---
title: "Go Bindings Public Contract"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: Python](../python/README.en.md) | [Next: Rust](../rust/README.en.md)
<!-- bindings-nav:end -->

# Go binding Core 0.13.0 public contract

> **What this chapter defines** — the public type, ownership, and error
> contract the currently implemented Go binding provides on top of the
> Core 0.13.0 raw C API.

This document defines only the public contract of the currently
implemented Go binding. It does not add pre-implementation designs or
features that exist only in other languages. Confirm the exact Go
identifiers and method signatures against `bindings/go/contracts/` and the
matching projection at the module root.

| Section | Covers |
|---|---|
| [Module and public package](#module-and-public-package) | Import path, the internal boundary, the Core 0.13.0 raw scope |
| [Public contract categories](#public-contract-categories) | A table of public concepts by category |
| [Context and resource lifetime](#context-and-resource-lifetime) | Ownership/release rules for Context/socket/monitor/poller/timer |
| [Byte HWM and Auto-HWM](#byte-hwm-and-auto-hwm) | Mapping between Go `uint64` and Core `uint64_t` byte HWM |
| [Message and ownership](#message-and-ownership) | Native storage, ownership per builder path |
| [Socket operation](#socket-operation) | Builder terminal signatures, per-socket operations |
| [Receive and eventing](#receive-and-eventing) | Caller-provided receive return values; monitor/poller/timer |
| [Receive flow state](#receive-flow-state) | The receive-flow state type, setter, and monitor surface |
| [Error contract](#error-contract) | The `ZlinkError` interface and concrete error types |
| [FFI and package boundary](#ffi-and-package-boundary) | The cgo include boundary, the module proxy layout |
| [What the public contract excludes](#what-the-public-contract-excludes) | The list of out-of-scope features |

## Module and public package

The Go module's import path is `zlink.systems/zlink`. A typical
consumer imports the `zlink` package at the module root.
`zlink.systems/zlink/contracts` is a public projection that declares
the same contract split by category, and the root package re-exports it.

Runtime handles, cgo declarations, native structs, callback trampolines, and
buffer marshalling are implementation
details of `internal/native`. These types and this package are not part of
the consumer contract.

- The current package contract projects only the Core 0.13.0 raw C API.
- It includes Context, Message, raw sockets, monitor, poller, timer, and utility, but not Spot, Actor, MeshNode, or service operations.
- The Go module has no per-message codec registration API either.
- The default path for messages and byte payloads uses the typed API the binding provides.

## Public contract categories

| Category | Main public concepts |
|------|----------------|
| Core | `Context`, `ContextOptions`, version/capability, `RoutingID`, utility |
| Messaging | `Message`, `Received`, `TopicMessage`, `SubscriptionEvent`, multipart helpers |
| Sockets | Pair, PUB, SUB, DEALER, ROUTER, XPUB, XSUB, STREAM, typed options, and operation builders |
| Eventing | `SocketMonitor`, `MonitorEvent`, `MonitorStatus`, `Poller`, `PollEvent`, `Timer` |
| Errors | Per-function-family error types, results, and result codes |

Socket features belong to their concrete socket type. The same method is
not forced onto every socket, and a socket without the matching raw Core
capability does not get that method.

## Context and resource lifetime

- The Context `NewContext` creates is the owner of its sockets and context-wide options.
- Closing a Context with `Close` propagates termination to any still-open sockets.
- Context, socket, monitor, poller, timer, and utility resources are owned by the caller, who calls `Close` or the matching close method once done.
- Calling Close repeatedly on the same resource does not re-release an already-closed state.

A context option sets the I/O thread count and socket defaults. Public `uint64`
methods preserve Core's full byte range for the Auto-HWM memory limit and Core
budget. The profile is passed as
`AutoHwmProfile`. Context provides
`CoreHwmBudgetSnapshot() (CoreHwmBudgetSnapshot, error)` and
`ResetCoreHwmBudgetMetrics() error`.

A socket or timer a poller has registered borrows that resource's handle.
The source must therefore be removed from the poller before it is
`Close`d, and the caller serializes add, modify, remove, and wait calls
against a single poller.

## Byte HWM and Auto-HWM

Core owns HWM calculation and queue admission. The Go binding validates the
`uint64` passed to `SetSendHighWaterMark(uint64)` and
`SetReceiveHighWaterMark(uint64)`, then preserves it in Core's 8-byte
`uint64_t` option. Getters return Core's full range as `uint64`. `0` means
unlimited.

The context passes the byte-valued memory limit and Core budget, plus the
canonical profile option, to Core. Core applies the profile ratio exactly once
and calculates planned byte HWM per physical directional queue. Setting a directional
HWM makes that direction a manual override and excludes it from automatic HWM
recalculation.

Input precedence is manual Core budget, explicit memory limit, a finite memory-
limit hint configured in the Go runtime, then Core fallback. Setting either of
the first two values disables automatic runtime-hint detection. The binding
does not combine the hint with Core's hard limit. If an explicit input exceeds
a finite hard limit Core detected, the binding preserves the existing
configuration error corresponding to `EINVAL` and does not clamp the value.

Core decides backpressure when the accounted bytes retained by a pipe reach
the applied HWM. The Go binding does not recount messages and passes Core's
result through the existing operation and error contract. Planned, applied,
and deferred HWM and in-flight usage in `MonitorStatus` are `uint64` bytes.
Pending-message counts remain display diagnostics; no slot, message-unit,
size-cap, or connection-bucket property is exposed.

## Message and ownership

`NewMessage` and `NewMessageWithSize` create native message storage owned
by Core. The input bytes to `NewMessage` are copied into native storage.
`Message.Data` returns a native payload view that is valid only while the
message stays open. When the lifetime needs to extend beyond the message,
`Message.Bytes` makes a snapshot.

| Builder path | Ownership rule |
|---|---|
| Adding a `Message` | Preserves the caller's message if the operation fails before Core admission and consumes it when admission succeeds |
| `MoveMessage` | Transfers ownership explicitly when `Submit` is called — no guarantee the caller can reuse the original message after it returns |
| `Bytes` | Reads the caller's slice while `Submit` runs and does not retain it after `Submit` returns |

The Go wrapper owns the `Message` parts in a receive result. Parts
delivered via `Received`, `TopicMessage`, `SubscriptionEvent`, or a request
completion channel are explicitly closed after use. When a `Recv` family
method takes caller-provided output, it clears that output object's
existing parts before filling in the new native parts and metadata.

Ordinary `Recv` and `Subscribe` return Core queue credit immediately when a
part is dequeued. The lifetime of an ordinary application receive result
therefore does not remain in HWM accounting. Only a Framework backend
explicitly selects the retained aggregate path below.

## Socket operation

### Builder terminal signature

Send, publish, request, and reply use a multipart builder. The builder
collects payload and the options allowed by that operation, then runs once at
the terminal `Submit`. Submitting the same builder twice completes the second
submission with a state error.

The current implementation's terminal signatures are as follows.

```go
// Message, MoveMessage, and Bytes add a payload part.
type SendSubmitOp interface {
    Message(*Message) SendSubmitOp
    MoveMessage(*Message) SendSubmitOp
    Bytes([]byte) SendSubmitOp
    Flags(SendFlags) SendSubmitOp
    Submit(context.Context) (bool, error)
}

// This is the HWM-managed routed submit used by DEALER Send and ROUTER SendTo.
// Submit is synchronous: blocking inside a goroutine is Go's idiomatic await,
// and the HWM wait itself is owned by Core.
type RoutedSendSubmitOp interface {
    Message(*Message) RoutedSendSubmitOp
    MoveMessage(*Message) RoutedSendSubmitOp
    Bytes([]byte) RoutedSendSubmitOp
    Submit(context.Context) error
}

// The single DEALER/ROUTER request terminal returns a completion channel.
type RequestSubmitOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
    Timeout(time.Duration) RequestSubmitOp
    Submit(context.Context) <-chan RequestReplyCompletion
}

type RequestReplyCompletion struct {
    Result RequestResult
    Parts  []*Message
    Err    error
}

// The reply builder Received.Reply() creates returns only an error on success, with no value.
type ReplySubmitOp interface {
    Message(*Message) ReplySubmitOp
    Flags(SendFlags) ReplySubmitOp
    Submit(context.Context) error
}
```

### DontWait and error classification

- Existing one-shot PAIR, PUB, XPUB, STREAM, and reply submits may use
  `SendFlagsDontWait` on builders that allow it.
- `ReplySubmitOp.Submit(ctx)` is a synchronous one-shot that returns no
  completion channel. It submits a terminal reply or error reply to the
  HWM-free completion lane with one native call. HWM backpressure is not a
  reply result; `NOT_CONNECTED`, `TERMINATED`, `INVALID_ARGUMENT`, and other
  non-HWM submit failures return immediately as a `*SubmitError` through
  `error`.
- Managed DEALER `Send`, ROUTER `SendTo`, and DEALER/ROUTER `Request` builders
  expose no flags, callback, or `SubmitAsync` compatibility terminal.
- **The binding owns no thread, no queue, and no retry.** A routed send's
  `Submit(ctx)` is a synchronous terminal that hands the complete record to a
  blocking Core call (`zlink_send_part` for DEALER, `zlink_send_part_rid` for
  ROUTER). The HWM wait happens entirely inside Core and resumes on a Core
  signal. There is no park queue, no readiness-callback retry, no deadline
  timer, and no dispatcher goroutine in the binding.
- The upper bound on that wait is the socket `SNDTIMEO`. `SNDTIMEO=0` is the
  `DONTWAIT` contract and fails immediately with
  `SubmitBackpressured`/`EAGAIN`. With an unbounded `SNDTIMEO` (`-1`) the
  calling goroutine waits inside Core until credit returns, so applications
  are advised to set a finite `SNDTIMEO`.
- For a routed send, `ctx` owns the **submit boundary**. An already-cancelled
  or already-expired `ctx` fails with `context.Canceled` /
  `context.DeadlineExceeded` and nothing reaches the wire. Once Core has taken
  the record, Core owns the wait and cancelling `ctx` does not interrupt it.
- A request's `Submit(ctx)` is a **synchronous submit with an asynchronous
  completion**. It snapshots one exact `(RID, transport pair, generation)`
  target (a policy-free value snapshot, not a credit reservation), submits
  through a blocking Core call, and returns the completion channel. The
  selected target does not change during the operation and detaching does not
  re-select another connection. The completion is driven by Core's reply
  handler callback — the binding adds no retry queue and no dedicated thread.
- The request timeout is Core-owned: the builder `Timeout`, or the socket's
  request-timeout option when absent, is handed to Core, and expiry is reported
  as `RequestTimedOut`. `ctx` cancellation and deadline separately complete the
  caller's channel first; a Core reply arriving afterwards is dropped and its
  parts released.
- Outbound paths on one native handle share a short record-attempt gate (a
  plain mutex) that protects one complete multipart attempt from its first part
  through `FINAL`. It is not a queue or a worker: it only prevents part-sequence
  interleaving and close races. While a blocking submit holds it inside Core,
  submits to other targets on the same socket serialize behind it.
- A request channel yields exactly one reply, submit failure, timeout,
  disconnect, or context-cancellation result and then closes. On success the
  caller closes `Parts`; failures are carried by `Err` and the corresponding
  `Result`.
- Payload parts from complete records submitted concurrently on the same
  socket do not interleave.
- On an existing one-shot send that returns a boolean, the normal result for
  temporary backpressure is `false, nil`; a real failure such as a broken
  connection, an invalid argument, or Core termination is returned as that
  function family's error. This `false, nil` rule does not apply to reply,
  which returns only an error.
- Only a non-blocking receive's no-data is represented as `false, nil`.
- Every other receive failure is an error.

### Per-socket operations

| Socket | Operations provided |
|---|---|
| PAIR | One-shot `Send` |
| PUB, XPUB | `Publish` |
| DEALER | Managed routed `Send` ending in a synchronous `Submit(ctx) error` |
| ROUTER | Managed routed `SendTo` taking a routing id and ending in a synchronous `Submit(ctx) error` |
| STREAM | A one-shot send operation that takes a target routing id |
| DEALER, ROUTER | A request operation — if ROUTER has received request metadata, it builds the reply operation from that metadata |
| STREAM | A raw TCP packet callback and caller-provided receive |

| Socket | Receive API |
|---|---|
| PAIR, DEALER, ROUTER, STREAM | Ordinary `Recv`, which fills `Received` storage; Framework-backend-only `RecvRetained` |
| SUB, XSUB | Ordinary `Subscribe`, which fills `TopicMessage` storage; Framework-backend-only `SubscribeRetained` |

Core's part functions are the internal substrate used to implement these
multipart receive APIs, and are not exposed as Go public methods.

## Receive and eventing

A caller-provided receive method returns `(bool, error)`. If `bool` is
`false`, it means there was no data to read under `RecvFlagsDontWait`, and
error is nil. If `bool` is `true`, the output has been filled with one or
more results. A real failure is `*RecvError`.

`RecvRetained(out, flags)` and `SubscribeRetained(out, flags)` preserve the
same `Received`/`TopicMessage` shape, routing ID, request sequence, topic, and
multipart framing as ordinary receive. Their only lifetime difference is
that the result privately owns one opaque Core retained credit for every
caller-visible physical payload part. These APIs let a Framework backend move
Core credit with the message through its queue, executor, and handler; they are
not the default application receive path.

`Received.Close` and `TopicMessage.Close` return the current parts and every
retained credit exactly once. Starting another ordinary or retained receive
with the same output first clears the old result, and no-data or a partial
multipart error leaves no credit already acquired behind. Framework drop,
cancellation, and error paths also call `Close` on the aggregate they own. The
Go binding does not use GC timing as a lifetime contract, so both normal
cleanup and leak prevention rely on explicit `Close` or reuse.

An individual `Message` part does not secretly own retained credit. The public
API exposes no native lease handle, separate retry/application capacity,
allowance, or duplicate accounting state.

A socket monitor is opened with a typed event mask and provides
`MonitorEvent` and `MonitorStatus`. Each Core 0.13.0 monitor event mask and
delivered event value is provided as its matching typed constant.
`MonitorEventMask` is used to open a monitor, and `MonitorEventType` is
used to check a received `MonitorEvent.Event`.
`OpenSocketMonitor(socket, options...)` accepts `MonitorEventMask` and
`MonitorHwmBytes(uint64)` as `MonitorOpenOption` values. With no event mask it
selects all events, and multiple masks are ORed. `MonitorHwmBytes(0)` selects
the Core default; a positive value is passed unchanged as the exact byte HWM.
If the option is supplied more than once, the last value in call order wins.
A `MonitorStatus` exposes `SndPendingBytes` and `RcvPendingBytes` separately
from pending-message counts. `CoreHwmBudgetSnapshot` projects ABI version/size,
configured/runtime/resolved memory limits, configured/effective budgets,
planned/applied/manual-reserved HWM, Core-queue/application/current/peak/
provisional accounted bytes, completion current/peak/pending and total-
messaging values, monitor/instance aggregates, application/completion queue
counts, `OutstandingApplicationLeaseCount`, `RetiredQueueCount`,
`DeferredOriginCreditBytes`, oversize/blocked/aggregate flags,
`BudgetGeneration`, and `MeasurementEpoch` as exact `uint64`/boolean values.
Reset preserves current, pending, queue-count, and those three owner-lifecycle
gauges, rebases both peaks to current, clears epoch counters, and increments
`MeasurementEpoch`. An ABI version/size mismatch is an unsupported error.
A poller reports the
readiness of a socket, file descriptor, or timer source as a `PollEvent`. A
timer is used to receive an interval event either via a poller or
directly. The callback or event result for a monitor, poller, or timer
never exposes the native callback thread as the execution location for a
public consumer callback.

## Receive flow state

The binding exposes the Core receive-flow state as the `ReceiveFlowState`
type with `ReceiveFlowRunning` and `ReceiveFlowPaused`.
`SetReceiveFlowState(ReceiveFlowState) error` sets it. It follows the Go error
contract: success is a `nil` error, and a failure is a `*ConfigError` whose
`Result` is the native `zlink_config_result_t` and whose errno is the native
errno, so a socket without a completion lane returns a `*ConfigError` with
`ConfigNotSupported`. A nil or closed handle returns `ConfigInvalidHandle`
without calling into Core. Setting the state the socket already holds returns
`nil`.

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

## Error contract

Every per-function-family error implements `error` and satisfies the
following public interface.

```go
type ZlinkError interface {
    error
    Code() int
    InternalErrno() int
}
```

- The current concrete error types are `SubmitError`, `RequestError`, `RecvError`, `HandlerError`, `CloseError`, `BindError`, `ConnectError`, and `ConfigError`.
- `Code()` returns that function family's Core result code, and `InternalErrno()` returns the native failure cause.
- `errors.Is` via `Unwrap()` is also supported.
- The `NativeErrno` field, and the `NativeErrno()` alias, are not part of the public contract.

- If a Context was already cancelled or past its deadline before `Submit`, a
  routed send channel yields that standard error, while a request channel puts
  it in `RequestReplyCompletion.Err`. The standard error is not converted into
  a per-function-family Core error.
- A reply or failure after native request admission is delivered through the
  same `RequestReplyCompletion` channel. There is no callback terminal.

## FFI and package boundary

The Go cgo bridge fixes its include path to `include/` inside the package.
A package consumer does not read the repository's `core/include` directly.
`bindings/go/tests/raw-core11-allowlist.json` fixes the header file set,
SHA-256, cgo raw symbols, and local callback helpers in a machine-readable
form. `zlink/service/` and earlier service symbols are not in the
allowlist.

The module package uses the following file proxy layout.

```text
zlink.systems/zlink/@v/v0.13.0.info
zlink.systems/zlink/@v/v0.13.0.mod
zlink.systems/zlink/@v/v0.13.0.zip
```

The supported platform runtimes are included under the module's
`native/<platform>/`. A package consumer should use the runtime from the
module cache, without `replace` and without the repository's `core/build`.

## What the public contract excludes

- Spot, Actor, MeshNode, and service operations
- Core 10 compatibility aliases and service headers
- Private cgo types, native pointers, and callback userdata
- A per-message codec registry, or a caller bypass to raw encode/decode
- `NativeErrno`

The current verification entry points for GoDoc and the process sample are
recorded in `bindings/go/README.godoc.md`, `bindings/go/tests/run_tests.sh`,
and `bindings/go/samples/run_samples.sh`. A public contract change in this
document is applied only after checking the common binding spec and the
review status of the related draft first.
