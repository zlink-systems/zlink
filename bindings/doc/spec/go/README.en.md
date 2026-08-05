---
title: "Go Bindings Public Contract"
---

<!-- bindings-nav:start -->
[Spec index](../README.md) | [Previous: Python](../python/README.md) | [Next: Rust](../rust/README.md)
<!-- bindings-nav:end -->

# Go binding Core 11 public contract

> **What this chapter defines** — the public type, ownership, and error
> contract the currently implemented Go binding provides on top of the
> Core 11 raw C API.

This document defines only the public contract of the currently
implemented Go binding. It does not add pre-implementation designs or
features that exist only in other languages. Confirm the exact Go
identifiers and method signatures against `bindings/go/contracts/` and the
matching projection at the module root.

| Section | Covers |
|---|---|
| [Module and public package](#module-and-public-package) | Import path, the internal boundary, the Core 11 raw scope |
| [Public contract categories](#public-contract-categories) | A table of public concepts by category |
| [Context and resource lifetime](#context-and-resource-lifetime) | Ownership/release rules for Context/socket/monitor/poller/timer |
| [Message and ownership](#message-and-ownership) | Native storage, ownership per builder path |
| [Socket operation](#socket-operation) | Builder terminal signatures, per-socket operations, ROUTER completion control |
| [Receive and eventing](#receive-and-eventing) | Caller-provided receive return values; monitor/poller/timer |
| [Error contract](#error-contract) | The `ZlinkError` interface and concrete error types |
| [FFI and package boundary](#ffi-and-package-boundary) | The cgo include boundary, the module proxy layout |
| [What the public contract excludes](#what-the-public-contract-excludes) | The list of out-of-scope features |

## Module and public package

The Go module's import path is `zlink.systems/zlink/v11`. A typical
consumer imports the `zlink` package at the module root.
`zlink.systems/zlink/v11/contracts` is a public projection that declares
the same contract split by category, and the root package re-exports it.

Runtime handles, cgo declarations, native structs, callback trampolines,
the request-progress pump, and buffer marshalling are implementation
details of `internal/native`. These types and this package are not part of
the consumer contract.

- The current package contract projects only the Core 11 raw C API.
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

A context option sets the I/O thread count and socket defaults. The
Auto-HWM message unit is passed as the `uint64` storage the Core contract
requires. A Go caller uses the public `int` method, but a negative value or
one outside the platform `uint64` range is rejected before it is set.

A socket or timer a poller has registered borrows that resource's handle.
The source must therefore be removed from the poller before it is
`Close`d, and the caller serializes add, modify, remove, and wait calls
against a single poller.

## Message and ownership

`NewMessage` and `NewMessageWithSize` create native message storage owned
by Core. The input bytes to `NewMessage` are copied into native storage.
`Message.Data` returns a native payload view that is valid only while the
message stays open. When the lifetime needs to extend beyond the message,
`Message.Bytes` makes a snapshot.

| Builder path | Ownership rule |
|---|---|
| Adding a `Message` | Preserves the caller's message on submit failure; consumes it on success |
| `MoveMessage` | Transfers ownership explicitly at submit time — no guarantee the caller can reuse the original message after it returns |
| `Bytes` | Reads the caller's slice during submit and does not retain the slice after submit returns |

The Go wrapper owns the `Message` parts in a receive result. Parts
delivered via `Received`, `TopicMessage`, `SubscriptionEvent`, or a request
completion callback are explicitly closed after use. When a `Recv` family
method takes caller-provided output, it clears that output object's
existing parts before filling in the new native parts and metadata.

## Socket operation

### Builder terminal signature

Send, publish, request, and reply use a multipart builder. The builder
collects payload and flags, then runs once at the terminal `Submit`.
Calling the same builder's terminal method twice has no guaranteed
behavior.

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

// Request submit chooses either a callback or a completion channel.
type RequestSubmitOp interface {
    Message(*Message) RequestSubmitOp
    Bytes([]byte) RequestSubmitOp
    Timeout(time.Duration) RequestSubmitOp
    Flags(SendFlags) RequestCallbackSubmitOp
    SubmitAsync(context.Context) (<-chan RequestReplyCompletion, error)
    Submit(context.Context, RequestReplyCallback) (bool, error)
}

// The reply builder Received.Reply() creates returns only an error on success, with no value.
type ReplySubmitOp interface {
    Message(*Message) ReplySubmitOp
    Flags(SendFlags) ReplySubmitOp
    Submit(context.Context) error
}
```

### DontWait and error classification

- `SendFlagsDontWait` avoids blocking.
- The normal result for temporary backpressure is `false, nil`; a real failure such as a broken connection, an invalid argument, or Core termination is returned as that function family's error.
- Only a non-blocking receive's no-data is represented as `false, nil`.
- Every other receive failure is an error.

### Per-socket operations

| Socket | Operations provided |
|---|---|
| PAIR, DEALER | `Send` |
| PUB, XPUB | `Publish` |
| ROUTER, STREAM | A send operation that takes a target routing id |
| DEALER, ROUTER | A request operation — if ROUTER has received request metadata, it builds the reply operation from that metadata |
| STREAM | A raw TCP packet callback and caller-provided receive |

| Socket | Receive surface |
|---|---|
| PAIR, DEALER, ROUTER, STREAM | `Recv`, which fills a `Received` storage |
| SUB, XSUB | `Subscribe`, which fills a `TopicMessage` storage |

Core's part functions are the internal substrate used to implement this
aggregate surface, and are not exposed as Go public methods.

### ROUTER completion control

ROUTER also provides the opaque multipart control record from Core's
completion connection. The `OnCompletionControl` handler receives a
`Received` carrying the source routing id and payload parts, and the
handler must close or consume those parts. The
`CompletionControl(peerRID)` builder sends a record to the specified peer
and rejects any flag other than `SendFlagsNone`.

## Receive and eventing

A caller-provided receive method returns `(bool, error)`. If `bool` is
`false`, it means there was no data to read under `RecvFlagsDontWait`, and
error is nil. If `bool` is `true`, the output has been filled with one or
more results. A real failure is `*RecvError`.

A socket monitor is opened with a typed event mask and provides
`MonitorEvent` and `MonitorStatus`. Each Core 11 monitor event mask and
delivered event value is provided as its matching typed constant.
`MonitorEventMask` is used to open a monitor, and `MonitorEventType` is
used to check a received `MonitorEvent.Event`. A poller reports the
readiness of a socket, file descriptor, or timer source as a `PollEvent`. A
timer is used to receive an interval event either via a poller or
directly. The callback or event result for a monitor, poller, or timer
never exposes the native callback thread as the execution location for a
public consumer callback.

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

- If a Context was already cancelled or past its deadline before the terminal method call, it returns `context.Canceled` or `context.DeadlineExceeded`.
- This standard error is not converted into a per-function-family Core error.
- A request completion result after a native submit has been accepted is delivered via `RequestReplyCompletion` or a callback's `RequestResult`.
- The unified policy for submit return rules and cancellation after completion is left as a separate review item until the Go/Rust submit draft's approval becomes the formal standard.

## FFI and package boundary

The Go cgo bridge fixes its include path to `include/` inside the package.
A package consumer does not read the repository's `core/include` directly.
`bindings/go/tests/raw-core11-allowlist.json` fixes the header file set,
SHA-256, cgo raw symbols, and local callback helpers in a machine-readable
form. `zlink/service/` and earlier service symbols are not in the
allowlist.

The module package uses the following file proxy layout.

```text
zlink.systems/zlink/v11/@v/v11.1.0.info
zlink.systems/zlink/v11/@v/v11.1.0.mod
zlink.systems/zlink/v11/@v/v11.1.0.zip
```

The supported platform runtimes are included under the module's
`native/<platform>/`. A package consumer should use the runtime from the
module cache, without `replace` and without the repository's `core/build`.

## What the public contract excludes

- Spot, Actor, MeshNode, and service operations
- Core 10 compatibility aliases and service headers
- Private cgo types, native pointers, callback userdata, and the progress pump
- A per-message codec registry, or a caller bypass to raw encode/decode
- `NativeErrno` and the earlier module path `zlink.systems/zlink`

The current verification entry points for GoDoc and the process sample are
recorded in `bindings/go/README.godoc.md`, `bindings/go/tests/run_tests.sh`,
and `bindings/go/samples/run_samples.sh`. A public contract change in this
document is applied only after checking the common binding spec and the
review status of the related draft first.
