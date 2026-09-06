---
title: "Runtime Boundary"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/08-runtime-boundary/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Utilities](07-utilities.en.md) | [Next: Socket Overview](socket/README.en.md)
<!-- zlink-nav:end -->

# Runtime Boundary

> **What this chapter defines** — The boundary that Core maintains exclusively for raw
> sockets and transports, and that higher layers must not cross.

## 1. Runtime boundary overview

This document defines the runtime boundary provided by the zlink Core public C ABI. Core is
a raw [socket](glossary.en.md#socket) runtime that encapsulates message transport and
operating-system I/O—a socket is an endpoint that sends and receives messages. Framework
owns application service topology and stateful object runtimes. The intended audience is
developers who implement or review Core and Framework runtimes and this boundary.

Responsibilities on the two sides of the boundary are divided as follows.

| Owner | Responsibility |
|---|---|
| Core | Provides, through its public C ABI, a raw socket runtime that encapsulates message transport and operating-system I/O. |
| Framework | Owns application service topology and stateful object runtimes, and implements service contracts using only the public raw socket API of each language binding. |
| Application (raw) | Uses raw sockets directly through the Core public C ABI or a language binding. If a transport liveness policy is needed, the application handles it through operating-system settings or its own application protocol ([§4](#4-transport-liveness-boundary)). |

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Socket creation, options, send, and receive | [Socket Common](socket/README.en.md) and each formal socket document |
| Context lifetime and options | [Context](01-context.en.md) |
| Message lifecycle and ownership | [Message](02-message.en.md) |
| Socket monitor contract | [Monitoring](06-monitoring.en.md) |
| Utilities such as pollers and timers | [Utilities](07-utilities.en.md) |
| Exclusion of the completion progress lane from HWM and budget accounting | [Auto HWM](systems/06-auto-hwm.en.md) |

## 2. Capabilities provided by Core

Core provides the following capabilities through its public C ABI.

- The lifecycle of [Context](glossary.en.md#context), the top-level container for I/O
  threads and sockets, and [I/O threads](glossary.en.md#io-thread), which perform network
  I/O
- Message allocation, ownership, multipart frames, and routing IDs
- PAIR, PUB, SUB, XPUB, XSUB, DEALER, ROUTER, and STREAM raw sockets
- Bind, connect, disconnect, endpoint, and connection lifecycle
- TCP, WebSocket, and TLS transports
- Classic PUB/SUB and raw STREAM
- Raw socket monitors, generic events, poll, and pollers
- Generic timers, threads, stopwatches, atomic counters, and proxies
- Request, handshake, and reconnect timeouts
- Receive-flow state for DEALER and ROUTER sockets

## 3. Capabilities owned by Framework

Core does not provide the following service concepts through its public C ABI, installed
headers, exported symbols, or compatibility facades.

- MeshName, ChannelName membership, and service discovery
- MeshNode lifecycle, peer admission, and node/channel messaging
- Ready batches, claims, receive batches, and reply tokens
- Spot, Actor, Instance Spot activation, and Logical Multicast
- Actor transfer, bound STREAM sessions, and service drain
- MeshNode monitors, service snapshots, and Spot-owned timers

The Core installation tree therefore has no `zlink/service/*.h`, and the root `zlink.h` does
not include a service header. Raw sockets have no ChannelName setter or getter. Generic
pollers handle only sockets, file descriptors, and generic timers; they return no service
owner or claim. Socket monitors report only transport and protocol state.

Framework runtimes implement service contracts using only the public raw socket API of each
language binding. There is no shared native service runtime for Framework, separate Core C
SPI, private binding entry point, or language-neutral service C ABI.

Core carries receive-flow state for DEALER-DEALER and DEALER-ROUTER in Core control frames
on the single Application connection. For ROUTER-ROUTER, it carries the state on the
[completion progress lane](glossary.en.md#completion-progress-lane) (a separate path that
also progresses terminal replies and error replies, hereafter the completion lane). The
runtime consumes both types of frame internally. [Auto HWM](systems/06-auto-hwm.en.md) owns
the contract that excludes the ROUTER-ROUTER completion lane from
[HWM](glossary.en.md#hwm) admission and the
[Auto HWM budget](glossary.en.md#auto-hwm-budget) (the byte total Core computes from memory
inputs and uses as the basis for dividing HWM among application queues). The public surface
for this
state consists of
`zlink_socket_set_receive_flow_state()` for configuration, three receive-flow monitor
events for observation, and the receive-flow fields of the monitor status snapshot. There
is no public API that receives, sends, encodes, or decodes a raw flow-state frame, and a
flow-state frame is not delivered to an application receive call. A binding therefore does
not create or interpret this frame directly.

## 4. Transport liveness boundary

Core reports orderly disconnects, transport failures, and protocol failures through socket
monitors and reconnects configured endpoints. A raw application that needs a policy for
detecting a half-open TCP connection configures operating-system TCP keepalive and the TCP
retransmission limit, or checks the state through its own application protocol.

Framework handles liveness messages for service connections, Location owner leases, and
STREAM session ping/pong. Core neither interprets these service messages nor determines
whether an application handler is able to process work.

The Core public option set does not include `ZLINK_OPT_HEARTBEAT_IVL`,
`ZLINK_OPT_HEARTBEAT_TTL`, or `ZLINK_OPT_HEARTBEAT_TIMEOUT`. The raw ZMP command set does
not include `zmp_control_heartbeat` or `zmp_control_heartbeat_ack`. Aliases, deprecated
options, and compatibility commands do not provide the same values.

## 5. Ownership and error boundary

The Core public specification defines the allocation, retain, copy, and close rules for raw
messages and socket handles. Framework follows the ownership contract exposed by each
binding and does not expose a Core-owned buffer view beyond the lifetime of an application
callback.

Core errors represent raw socket, transport, protocol, and operating-system failures.
Framework maps these errors to typed terminal results for service operations. It does not
promote Core error codes directly into the Framework application contract or add service
retry policy to Core.

Core does not determine the progress of accepted service work, handler completion, Actor
transfer, checkpoints, or host termination. Each language Framework runtime separates raw
I/O progress from application dispatch progress.

## 6. Internal structure

> **Contract ownership for this section** — The contract sections of this document
> ([§2](#2-capabilities-provided-by-core)–[§5](#5-ownership-and-error-boundary)) and the
> [verification requirements](#7-implementation-and-contract-test-verification-requirements)
> own the public boundary maintained by Core. This section explains how the internal layers
> divide responsibilities to enforce that boundary.

Core 0.13.0 implements only raw sockets and transports. The public API facade validates
arguments, handles, and ownership. The socket semantics layer determines routing for
PAIR, PUB/SUB, DEALER/ROUTER, and STREAM. Runtime core manages connections, sessions,
pipes, and I/O threads, while engines handle TCP, WebSocket, and TLS framing.

```text
+------------------------------+
| Public C API                 |
+------------------------------+
| Raw Socket Semantics         |
+------------------------------+
| Runtime Core and Pipes       |
+------------------------------+
| Engines and Transports       |
+------------------------------+
```

Core source and public ABI contain no mesh topology, application mailbox, stateful object,
discovery authority, or service lifecycle. Framework runtimes use the public raw API of
each language binding.

### Public binding boundary

Core public headers provide only context, message, raw socket, endpoint, option, poller,
timer, and monitor contracts. The C++, .NET, JVM, and Node.js bindings project this raw
contract into their languages. Framework runtimes use only the installed binding's public
API.

There is no Framework-only service C ABI, private callback SPI, shared native service
runtime, or separate loader. If a raw capability is insufficient, maintainers first assess
whether the primitive is also required by generic raw socket users, and then update the
public Core specification and all four binding contracts together. Framework does not use
Core private headers or unexported symbols directly.

### Socket and pipe ownership

Context owns I/O threads and global runtime resources. A socket owns its options, endpoints,
sessions, and pipes. A session manages one transport connection's protocol engine and
reconnect state, while a pipe manages message flow between a socket queue and an engine.

Socket close blocks new sends, receives, and callback registration, terminates sessions and
pipes, and then invalidates the handle. Engine timers and monitor events belong to the
corresponding connection lifetime. An engine callback that arrives late after close does
not modify the terminated socket state.

A Core connection identity is a raw observable value that distinguishes a physical
lifetime. Core does not interpret it as a Mesh lifecycle generation, descriptor revision,
Actor authority owner generation, or Location authority store version.

A DEALER-ROUTER logical peer uses one Application transport connection. DATA, REQUEST,
REPLY, and error reply share the same physical FIFO, Application HWM, and PAUSED state. If
the DEALER does not dequeue DATA sent first by the ROUTER, a later REPLY cannot overtake it
and the request timeout can complete first. DEALER-DEALER also uses one Application
connection and does not allow typed requests.

A ROUTER-ROUTER logical peer uses Application and Completion transport connections. Before
allowing Application writes, Core validates the pair ID, pair generation, lane, and peer
identity of both connections. Failure of either lane terminates both lanes. Ordinary
messages and requests use the Application lane, while replies use the Completion lane.
Therefore, requests already sent can complete even while ROUTER-ROUTER Application ingress
is stopped by [backpressure](glossary.en.md#backpressure) (behavior that limits additional
submissions from a sender when receiving cannot keep pace with processing).

Received application payload moves from the directional network pipe straight to the public
receive without passing through a hidden PAIR queue. A REQUEST's reply payload is owned by the
socket-local completion record until it is received or discarded, and a successful completion
receive transfers that ownership to the caller without a copy — a contract owned by the
[socket README's completion ownership](socket/README.en.md#completion-pull-and-ownership). This
record queue is neither a transport lane nor a wire record.

### Transport liveness implementation

TCP and WebSocket engines deliver orderly disconnects, read/write failures, and protocol
failures to the session. The session reports them through the socket monitor and updates
the reconnect state of configured endpoints. Operating-system TCP keepalive and the TCP
retransmission limit are applied as transport options; the engine does not create separate
application control frames.

Framework service protocol liveness messages are carried as raw application payload. Core
does not interpret their body or deadline; each language Framework runtime processes them
through its infrastructure queue and scheduler.

The Core source boundary contains no `ZLINK_OPT_HEARTBEAT_IVL`,
`ZLINK_OPT_HEARTBEAT_TTL`, `ZLINK_OPT_HEARTBEAT_TIMEOUT`,
`zmp_control_heartbeat`, `zmp_control_heartbeat_ack`, or codec, parser, and engine state
that handles those values. It also contains no `heartbeat_ivl_timer_id`,
`heartbeat_ttl_timer_id`, `heartbeat_timeout_timer_id`, or corresponding callback branches.
Generic engine timers and reconnect timers are raw transport resources.

### Timer boundary

Core timers are monotonic scheduling primitives required for raw socket engines, reconnect,
and poller integration. A timer knows nothing about application Spot turns, Actor
lifecycles, Instance leases, or transfer phases. Framework object timers and deadline
schedulers are implemented using the binding's public timer and poller APIs or the
corresponding language scheduler.

When a timer owner terminates, it cancels callback registration and prevents pending
callbacks from referring to the owner state again. A timer ID has meaning only within the
lifetime of the same owner and is not used as a Framework operation ID or generation.

### Monitor boundary

Core monitors report raw bind, accept, connect, disconnect, retry, protocol, and transport
failures. An event describes a raw socket and connection lifetime; it does not include
MeshName, ChannelName, peer admission, Spot, Actor, Location Store, or host termination
results.

Framework receives raw events through the binding's public monitor API and applies them to
its own peer registry and state reducer. The Core raw monitor queue and Framework typed
observer queue are separate resources. Core monitors do not handle slow consumption,
coalescing, or metric policy for Framework observers.

### Raw-only invariants

- Core source and public ABI own no service protocol command or state machine.
- Core creates no application mailbox, ready owner, claim, reply token, or terminal request
  state.
- Core does not interpret Spot, Actor, or Instance identity, generation, or activation
  barriers.
- Core does not call a Location Store, Checkpoint Store, lease, owner CAS, or maintenance
  recovery operation.
- Raw engine timers and raw monitors are connection resources.
- Framework uses only public binding APIs and does not depend on private Core symbols.
- Raw socket options and monitor events are not passed through unchanged to public Framework
  service APIs.

## 7. Implementation and contract-test verification requirements

Verify the following using only the public surface: the installed header tree, exported
symbols, public option and command inventories, and the send, receive, and monitor results
of public APIs. Each item maps to one check.

**Installation surface and symbols**

- The installation tree and exported symbols contain no service headers, types, or
  functions—the Core installation tree has no `zlink/service/*.h`, and the root `zlink.h`
  does not include a service header.
- `zlink_socket_set_channel_name`, `zlink_socket_get_channel_name`, MeshNode poll and
  monitor support, and `zlink_spot_timer_new` are absent.
- Public headers and exported symbols contain no Framework-only C SPI or service
  compatibility facade.

**Option and command inventory**

- The raw option and ZMP command inventories agree with the formal socket and protocol
  specifications.
- The public option set does not include `ZLINK_OPT_HEARTBEAT_IVL`,
  `ZLINK_OPT_HEARTBEAT_TTL`, or `ZLINK_OPT_HEARTBEAT_TIMEOUT`; the raw ZMP command set does
  not include `zmp_control_heartbeat` or `zmp_control_heartbeat_ack`; and aliases,
  deprecated options, and compatibility commands do not provide the same values.

**Raw surface behavior**

- Raw socket, generic poller and timer, and socket monitor contract tests pass.
- Generic pollers handle only sockets, file descriptors, and generic timers; they return no
  service owner or claim.
- Socket monitors report only transport and protocol state.

**Receive-flow state**

- The public surface for receive-flow state consists only of
  `zlink_socket_set_receive_flow_state()`, three receive-flow monitor events, and the
  receive-flow fields of the monitor status snapshot.
- There is no public API that receives, sends, encodes, or decodes a raw flow-state frame,
  and a flow-state frame is not delivered to an application receive call.
- DEALER-ROUTER uses one Application connection, while ROUTER-ROUTER uses Application and
  Completion connections. Both topologies expose exactly one `CONNECTION_READY` per logical
  peer.
- Regardless of this topology difference, Framework runtimes use only the public Core raw
  socket API and do not create or interpret raw FLOWSTATE directly.

**Framework boundary**

- Framework runtimes use only the public Core raw surface.
- Core public APIs and implementation contain no ChannelName, service dispatch, Spot,
  Actor, transfer, or maintenance semantics.

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Utilities](07-utilities.en.md) | [Next: Socket Overview](socket/README.en.md)
<!-- zlink-nav:end -->
