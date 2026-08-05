---
title: "Python Bindings Public Contract"
---

<!-- bindings-nav:start -->
[Spec index](../README.md) | [Previous: Node.js](../node/README.md) | [Next: Go](../go/README.md)
<!-- bindings-nav:end -->

# Python binding Core 11 public contract

> **What this chapter defines** — the public type, ownership, and error
> contract the `zlink` Python package provides on top of Core 11 raw
> messaging.

- This document defines the Core 11 raw messaging contract the `zlink` Python package provides.
- A feature not in the current implementation and public header is not part of this contract.
- It supports Python 3.9 and later; the current candidate package version is `11.2.0`.
- The current native package target is Linux x86_64; other targets are outside this contract's supported scope until a separate candidate payload and clean-consumer verification exist.

| Section | Covers |
|---|---|
| [Scope](#scope) | The list of public Python types that represent Core resources |
| [Package surface](#package-surface) | The boundary between the public factories and the private area |
| [Ownership and lifetime](#ownership-and-lifetime) | Ownership/release rules for native handles, messages, and Received |
| [Callback surface](#callback-surface) | The public callback paths, and the primitives that stay unexposed |
| [Send/receive and no-data](#sendreceive-and-no-data) | How submit and no-data are represented, and how native failures are delivered |
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

## Ownership and lifetime

- `Context` owns the native context and releases it via `close()` or context-manager exit.
- A socket, monitor, poller, or timer owns the native handle it creates. A call that uses the handle after a successful `close()` is not allowed.
- `Message.from_(value)` creates a native message independent of the caller's value. After a successful send submit, native ownership of the message part moves to the Core send path.
- `Received` is a receive storage the caller creates. On a successful `recv_into(received)`, the parts and routing metadata are recorded into `Received`; native parts are released on `close()` or context-manager exit.
- The native view `Received`'s `parts` provides is valid only while its owner stays open. If it must outlive that, copy the value with `to_bytes()` or `to_bytes_list()`.
- Once a callback is registered, the callback and any Python references it needs are not released before the native callback registration is. A callback exception is delivered per the binding's callback error policy.

## Callback surface

Core FFI's `zlink_recv_handler()` and
`zlink_router_completion_control_handler()` are private implementation
primitives the Python package does not expose directly. Python's public
callback surface is fixed to `on_packet` for a STREAM packet,
`on_send_ready` for send readiness, `on_event` for a monitor event, and the
`request(...)` callback path that delivers ROUTER request completion. It
provides no separate public method to register a raw receive callback or a
completion-control handler.

## Send/receive and no-data

- A send builder adds message parts, then calls `submit()`. A blocking send follows the socket option and Core's timeout contract.
- A caller-provided receive using `RecvFlags.DONT_WAIT` returns `False` when there is no message.
- A direct-return control API such as a timer or monitor returns `None` when there is no pending value.
- An actual native failure is delivered as its matching error type, never hidden as no-data.

DEALER and ROUTER request/reply preserve Core's routing metadata and
request sequence. A ROUTER receive's `Received.routing_id` is the raw
routing id and is never converted to a different identity type. The
current single-part accessor name matches the implementation and contract
tests: `single_part_or_throw()`. A name change happens only after a
separate draft is approved.

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

- Usage follows the [Python guide](../../guide/python/index.ko.md).
- The reference for Core functions and layout is the repository's `core/include/zlink.h` and the Core spec.
- Implementation detail and callback/native lifetime explanations belong to the internals documents, not this one.
