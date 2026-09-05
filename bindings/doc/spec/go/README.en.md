---
title: "Go Bindings Public Contract"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: Python](../python/README.en.md) | [Next: Rust](../rust/README.en.md)
<!-- bindings-nav:end -->

# Go binding Core 0.16.0 public contract

> **What this chapter defines** — the public type, ownership, and error
> contract the Go binding provides on top of the Core 0.16.0 raw C API.

This document defines the Go binding's public contract. Features that exist only in other languages
are not part of this contract. Confirm the exact Go
identifiers and method signatures against `bindings/go/contracts/` and the
matching projection at the module root.

| Section | Covers |
|---|---|
| [Module and public package](#module-and-public-package) | Import path, the internal boundary, the Core 0.16.0 raw scope |
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

Runtime handles, cgo declarations, native structs, completion-drain state, and
buffer marshalling are implementation
details of `internal/native`. These types and this package are not part of
the consumer contract.

- The package contract projects the Core 0.16.0 raw C API.
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

`(*Poller).AddMonitor(monitor *SocketMonitor, events PollEventFlag, slot uintptr) error`,
`(*Poller).ModifyMonitor(monitor *SocketMonitor, events PollEventFlag) error` and
`(*Poller).RemoveMonitor(monitor *SocketMonitor) error` register, modify and remove a socket monitor as a poller
source (common spec "Monitor sources in `Poller`"); the existing `AddSocket/ModifySocket/RemoveSocket`
(`SocketTarget`) also accept a monitor. Only `POLLIN` is valid for a monitor; any other bit is rejected with a
typed `ConfigResult` `InvalidArgument`. Drain with `monitor.Recv(RecvFlagsDontWait)` after readiness; the poll event reports
the monitor through the same slot and source kind as a socket.

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
delivered via `Received`, `TopicMessage`, `SubscriptionEvent`, or a successful request result are
explicitly closed after use. When a `Recv` family
method takes caller-provided output, it clears that output object's
existing parts before filling in the new native parts and metadata.

Core byte-HWM charge ends when ordinary `Recv` or `Subscribe` dequeues a part.
The lifetime of an application receive result therefore does not remain in HWM
accounting.

## Socket operation

Send, publish, request, and reply use multipart builders. A builder collects its payload and the
options allowed for that operation, then executes once at the terminal `Submit`. Submitting the same
builder twice completes the second submission with a state error.

Send and request `Submit(context.Context)` wait for a Core `DONTWAIT` completion. Reply checks the
Context before call entry, and socket `SNDTIMEO` owns the admission wait after the native call. Only
publish provides `Flags(SendFlags)`, on a separate `PublishOp`. The
[Pull completion public contract](#pull-completion-public-contract) contains the exact interface.

### Context and error classification

- A Context canceled or past its deadline before the call fails with `context.Canceled` or
  `context.DeadlineExceeded` without starting a native operation.
- Context cancellation after successful submit ends only the caller wait. The runtime continues draining
  the native completion and does not deliver the request result again.
- Parts of multipart records submitted concurrently on the same socket do not interleave. When the public
  API preserves a caller message, binding staging restores it; the binding creates no retransmission queue.
- Only no-data from non-blocking receive is represented as `false, nil`; every other receive failure is
  an error.

### Per-socket operations

| Socket | Operations |
|---|---|
| PAIR | `Send()` |
| PUB, XPUB | `Publish(topic)`, returning a separate `PublishOp` |
| DEALER | `Send()`, `Request()` |
| ROUTER | `SendTo(RoutingID)`, `Request(RoutingID)`, `Reply(RoutingID, ReplyToken)` |
| STREAM | `SendTo(RoutingID)`, RAW `Recv`, PACKET `RecvPacket` |

| Socket | Receive API |
|---|---|
| PAIR, DEALER, ROUTER, STREAM | `Recv` filling `Received` storage |
| SUB, XSUB | `Subscribe` filling `TopicMessage` storage |

Core part functions are the internal substrate for these multipart receive APIs and are not exposed as
public Go methods.

## Receive and eventing

A caller-provided receive method returns `(bool, error)`. If `bool` is
`false`, it means there was no data to read under `RecvFlagsDontWait`, and
error is nil. If `bool` is `true`, the output has been filled with one or
more results. A real failure is `*RecvError`.

Core byte-HWM charge ends when `Recv` or `Subscribe` dequeues the payload.
`Received` and `TopicMessage` own only the Go lifetime of parts, routing ID,
`ReplyToken`, topic, and multipart framing. `Close` and storage reuse clean
up that payload and metadata but do not participate in Core HWM accounting. No
separate retained receive, native lease handle, application byte capacity, or
duplicate accounting state exists in a public or internal API.

A socket monitor is opened with a typed event mask and provides
`MonitorEvent` and `MonitorStatus`. Each Core 0.16.0 monitor event mask and
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
`ApplicationAccountedBytes` and those three owner-lifecycle fields are
ABI-reserved and always zero. Reset preserves current, pending, and queue-count
gauges, rebases both peaks to current, clears epoch counters, and increments
`MeasurementEpoch`. An ABI version/size mismatch is an unsupported error.
A poller reports the readiness of a socket, file descriptor, or timer source as a `PollEvent`. A timer
is used to receive an interval event either via a poller or directly. Public pull methods on monitor and
timer return events and fire counts.

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
and `FLOW_STATE_STALE_EPOCH` (`1 << 3`), the status detail bit `FLOW_STATE`
(`1 << 5`), and the five status
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

- If a Context was already canceled or past its deadline before `Submit`, that standard error is
  returned. It is not converted into a per-function-family Core error.
- A reply or failure after native request admission is returned through the
  `([]*Message, error)` result of `Submit(context.Context)`.

## FFI and package boundary

The Go cgo bridge fixes its include path to `include/` inside the package.
A package consumer does not read the repository's `core/include` directly.
`bindings/go/tests/raw-core11-allowlist.json` fixes the header file set,
SHA-256, cgo raw symbols, and local native helpers in a machine-readable
form. `zlink/service/` and earlier service symbols are not in the
allowlist.

The module package uses the following file proxy layout.

```text
zlink.systems/zlink/@v/v0.16.0.info
zlink.systems/zlink/@v/v0.16.0.mod
zlink.systems/zlink/@v/v0.16.0.zip
```

The supported platform runtimes are included under the module's
`native/<platform>/`. A package consumer should use the runtime from the
module cache, without `replace` and without the repository's `core/build`.

## What the public contract excludes

- Spot, Actor, MeshNode, and service operations
- Core 10 compatibility aliases and service headers
- Private cgo types and native pointers
- A per-message codec registry, or a caller bypass to raw encode/decode
- `NativeErrno`

The verification entry points for GoDoc and the process sample are
recorded in `bindings/go/README.godoc.md`, `bindings/go/tests/run_tests.sh`,
and `bindings/go/samples/run_samples.sh`.

## Pull completion public contract

The Go module uses Core 0.16.0 as an exact dependency.

Go uses one `Submit(context.Context)` terminal. The runtime submits with Core `DONTWAIT` and waits the
goroutine until the socket-local owner drains the completion. A `Context` ends only the caller wait; it
does not cancel the native operation. If request cancellation is decided first, submit returns
`(nil, ctx.Err())`, and a late completion releases its payload.

`PollCompletion` is a progress event indicating that the public poller's wait goroutine drained the
native queue and fully processed at least one live waiter or detached state. Under public poller
ownership, another goroutine must continue executing the wait loop while `Submit(ctx)` is in use.

A ROUTER REQUEST receive creates a `ReplyToken` as a struct literal inside the package. Its zero value is
invalid, and equality compares both the owner pointer and opaque value. The zero value of `StreamPacket`
is an empty reusable output. Publish preserves its existing flags and synchronous submit result on a
`PublishOp` separate from send. A token provides no raw accessor, ordering, serialization, or `Close`.
Concurrent recv into the same output is invalid-state. Message pointers remain valid only until the next
recv entry or `Close()`. Before the first bind/connect, `SetReceiveMode` accepts only
`StreamReceiveRaw` and `StreamReceivePacket` and rejects `StreamReceiveUnspecified`.

### Public interface

```go
type SendOp interface {
    Message(*Message) SendSubmitOp
    MoveMessage(*Message) SendSubmitOp
    Bytes([]byte) SendSubmitOp
}

type SendSubmitOp interface {
    Message(*Message) SendSubmitOp
    MoveMessage(*Message) SendSubmitOp
    Bytes([]byte) SendSubmitOp
    Submit(context.Context) error
}

type RequestOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
}

type RequestSubmitOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
    Timeout(time.Duration) RequestSubmitOp
    Submit(context.Context) ([]*Message, error)
}

type ReplyOp interface {
    Message(*Message) ReplySubmitOp
}

type ReplySubmitOp interface {
    Message(*Message) ReplySubmitOp
    Submit(context.Context) error
}

type PublishOp interface {
    Message(*Message) PublishSubmitOp
    MoveMessage(*Message) PublishSubmitOp
    Bytes([]byte) PublishSubmitOp
}

type PublishSubmitOp interface {
    Message(*Message) PublishSubmitOp
    MoveMessage(*Message) PublishSubmitOp
    Bytes([]byte) PublishSubmitOp
    Flags(SendFlags) PublishSubmitOp
    Submit(context.Context) (bool, error)
}

type ReplyToken struct {
    owner *replyTokenOwner
    value uint64
}

func (r *Received) ReplyToken() (ReplyToken, bool)
func (s *RouterSocket) Reply(
    rid RoutingID, token ReplyToken) ReplyOp

type StreamReceiveMode int32

const (
    StreamReceiveUnspecified StreamReceiveMode = iota
    StreamReceiveRaw
    StreamReceivePacket
)

type StreamPacket struct { /* unexported reusable state */ }

func (p *StreamPacket) Empty() bool
func (p *StreamPacket) RoutingID() RoutingID
func (p *StreamPacket) HasRoutingID() bool
func (p *StreamPacket) Header() *Message
func (p *StreamPacket) Body() *Message
func (p *StreamPacket) Close() error

func (s *StreamSocket) RecvPacket(
    out *StreamPacket, flags RecvFlags) (bool, error)
func (s *StreamSocket) ReceiveMode() (StreamReceiveMode, error)
func (s *StreamSocket) SetReceiveMode(StreamReceiveMode) error
```

The operation-start signatures are PAIR `Send() SendOp`, DEALER `Send() SendOp` and
`Request() RequestOp`, ROUTER `SendTo(RoutingID) SendOp`, `Request(RoutingID) RequestOp`, and
`Reply(RoutingID, ReplyToken) ReplyOp`, and STREAM `SendTo(RoutingID) SendOp`.
`Received.Send()` and `Received.Reply()` capture the source target and token. `PubSocket.Publish(topic)`
and `XPubSocket.Publish(topic)` return `PublishOp`. Calling `Received.Reply()` on a DATA envelope
returns a state error.

The public Go surface contains no send/request/reply `Flags`, `RequestSyncSubmitOp`, completion channel,
`RequestReplyCompletion`, `Received.RequestSeq`, STREAM/monitor/timer callback, pair/generation member,
`SocketMonitor.OnEvent`, or `Timer.OnFire`. `RoutedSendOp` and `RoutedSendSubmitOp` are not public types.

Monitor provides `Recv(RecvFlags) (*MonitorEvent, error)`, `Status()`, and `Close()`. Timer provides
`Start(intervalNs, repeatCount uint64)`, `Stop()`, `Recv() (uint64, bool, error)`, and `Close()`. Monitor
DONTWAIT no-data is distinguished by `NO_DATA` on `*RecvError`. The native-header mirror contains only
`ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` as pending options. Monitor-event
`ConnectionID` is used only for diagnostics and correlation, not as a send/reply target or reconnect
fence. Pending native options add no public high-level option method.

## Implementation and contract-test verification requirements

Verify the following using only the public Go interface, return values, and poller events. Each item
maps to one contract test.

**Operations and completion**

- Send and request provide one `Submit(context.Context)` terminal. Request success returns
  `([]*Message, nil)`, and a non-OK completion returns `(nil, typed request error)`.
- Send flags not shared by Go and Python appear only on `PublishSubmitOp`, and publish submit retains its
  `(bool, error)` result.
- Even when completion drains before submit returns, the result is returned exactly once after joining
  submit publication.
- If Context cancellation is decided first, submit returns `(nil, ctx.Err())`; a late completion does
  not deliver the result again.
- Under public poller ownership, `Submit(ctx)` progresses when another goroutine executes `wait()`.

**ReplyToken and STREAM**

- `Received.ReplyToken()` returns a valid token and `true` for ROUTER REQUEST, and a zero token and
  `false` for DATA.
- A zero token and a token from another owner fail before reply-builder creation.
- A zero-value `StreamPacket` is empty. Its accessors remain empty after `RecvPacket()` no-data or error
  and after `Close()`, and the output can be reused.

**Pull eventing**

- Monitor and timer recv return events and fire counts without handlers and distinguish their respective
  no-data results.
