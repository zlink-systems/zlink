[한국어](08-runtime-boundary.ko.md) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Utilities](07-utilities.en.md) | [Next: Socket Overview](socket/README.en.md)
<!-- zlink-nav:end -->

# Core Runtime Boundary

This document defines the public C ABI boundary of ZLink Core. Core is a
raw-socket runtime that encapsulates message transport and operating-system I/O.
Framework owns application service topology and stateful-object runtimes.

## 1. Core capabilities

Core provides the following capabilities through its public C ABI:

- Context and I/O-thread lifecycle
- Message allocation, ownership, multipart frames, and routing IDs
- PAIR, PUB, SUB, XPUB, XSUB, DEALER, ROUTER, and STREAM raw sockets
- Bind, connect, disconnect, endpoint, and connection lifecycle
- TCP, WebSocket, and TLS transports
- Classic PUB/SUB and raw STREAM
- Raw-socket monitoring, generic events, poll, and pollers
- Generic timers, threads, stopwatches, atomic counters, and proxies
- Request, handshake, and reconnect timeouts
- Paired DEALER/ROUTER receive-flow state

## 2. Framework-owned capabilities

Core does not provide the following service concepts through its public C
ABI, installed headers, exported symbols, or compatibility facades:

- MeshName, ChannelName membership, and service discovery
- MeshNode lifecycle, peer admission, and node/channel messaging
- Ready batches, claims, receive batches, and reply tokens
- Spots, Actors, Instance Spot activation, and Logical Multicast
- Actor transfer, bound STREAM sessions, and service drain
- MeshNode monitors, service snapshots, and Spot-owned timers

The Core installation therefore has no `zlink/service/*.h` headers, and the root
`zlink.h` does not include a service header. Raw sockets have no ChannelName
setter or getter. Generic pollers handle only sockets, file descriptors, and
generic timers; they return no service owner or claim. Socket monitors report
only transport and protocol state.

Framework runtimes implement service contracts using only the public raw-socket
API of each language binding. The design does not add a shared native service
runtime, a separate Core C SPI, a private binding entry point, or a
language-neutral service C ABI.

Core carries the receive-flow state of a paired DEALER/ROUTER socket in frames
on the completion lane and consumes those frames inside the runtime. The public
surface for that state is `zlink_socket_set_receive_flow_state()` for setting
it, the three receive-flow monitor events for observing it, and the
receive-flow fields of the monitor status snapshot. There is no public API that
receives, sends, encodes, or decodes a raw flow-state frame, and a flow-state
frame is never delivered to an application receive call. A binding therefore
never builds or parses one.

## 3. Transport-liveness boundary

Core reports orderly disconnects, transport failures, and protocol failures
through socket monitors and reconnects configured endpoints. A raw application
that needs to detect a half-open TCP connection configures operating-system TCP
keepalive and the TCP retransmission limit, or defines that check in its own
application protocol.

Framework implements service-connection liveness messages, Location-owner
leases, and STREAM-session ping/pong. Core neither interprets those service
messages nor decides whether an application handler can process work.

The Core public-option set does not include `ZLINK_OPT_HEARTBEAT_IVL`,
`ZLINK_OPT_HEARTBEAT_TTL`, or `ZLINK_OPT_HEARTBEAT_TIMEOUT`. The raw ZMP command
set does not include `zmp_control_heartbeat` or `zmp_control_heartbeat_ack`.
Aliases, deprecated options, and compatibility commands do not expose the same
values.

## 4. Ownership and error boundary

The Core public specification owns allocation, retain, copy, and close rules
for raw messages and socket handles. Framework follows the ownership contract
published by each binding and does not expose a Core-owned buffer view beyond
the lifetime of an application callback.

Core errors describe raw-socket, transport, protocol, and operating-system
failures. Framework maps them to typed terminal results for service operations.
It does not promote Core error codes directly into the Framework application
contract or add service retry policy to Core.

Core does not decide progress for accepted service work, handler completion,
Actor transfer, checkpoints, or host termination. Each language Framework
runtime separates raw-I/O progress from application-dispatch progress.

## 5. Public-surface verification

Core public-surface verification must establish all of the following:

- The install tree and exported symbols contain no service headers, types, or functions.
- `zlink_socket_set_channel_name`, `zlink_socket_get_channel_name`, MeshNode poll
  and monitor support, and `zlink_spot_timer_new` are absent.
- Public headers and exported symbols contain no Framework-only C SPI or service
  compatibility facade.
- Raw-socket, generic poller/timer, and socket-monitor contract tests pass.
- The raw-option and ZMP-command inventories agree with the formal socket and
  protocol specifications.
- Framework runtimes use only the public Core raw surface.
- Core public APIs and implementation contain no ChannelName, service dispatch,
  Spot, Actor, transfer, or maintenance semantics.

## Internals

> **The document that owns this chapter's contract** — the public boundary
> Core maintains is covered by the contract part of this document. This
> section explains how the internal layers actually divide and enforce that
> boundary.

Core implements only raw sockets and transports. The public API facade
validates arguments, handles, and ownership. Socket semantics implement routing
for PAIR, PUB/SUB, DEALER/ROUTER, and STREAM. Runtime core manages connections,
sessions, pipes, and I/O threads, while engines implement TCP, WebSocket, and
TLS framing.

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

Core source and public ABI contain no mesh topology, application mailbox,
stateful object, discovery authority, or service lifecycle. Framework runtimes
use only the public raw API of each language binding.

### Public binding boundary

Core public headers expose only context, message, raw socket, endpoint, option,
poller, timer, and monitor contracts. The C++, .NET, JVM, and Node.js bindings
project that raw contract into their languages. Framework runtimes use only the
installed public API of the corresponding binding.

The design does not add a Framework-only service C ABI, private callback SPI,
shared native service runtime, or separate loader. If a raw capability is
missing, maintainers first determine whether it is a generally useful raw
socket primitive and then update the public Core specification and all four
binding contracts. Framework does not call private Core headers or unexported
symbols.

### Socket and pipe ownership

The context owns I/O threads and global runtime resources. A socket owns its
options, endpoints, sessions, and pipes. A session manages one transport
connection's protocol engine and reconnect state. A pipe carries messages
between a socket queue and an engine.

Socket close first blocks new send, receive, and callback registration, then
terminates sessions and pipes, and finally invalidates the handle. Engine timers
and monitor events belong to one connection lifetime. A late engine callback
cannot modify a socket that has already closed.

A Core connection identity distinguishes a physical lifetime. Core does not
interpret it as a mesh lifecycle generation, descriptor revision, Actor
authority-owner generation, or Location authority store version.

For DEALER/ROUTER request-reply, one logical peer owns an Application and a
Completion transport connection. Core validates their pair ID, pair generation,
lane, and peer identity before it releases Application writes. Failure of one
lane terminates both lanes. Ordinary messages and requests use the Application
lane; replies use the Completion lane. This keeps request completion available
when Application ingress is backpressured.

Each lane retains payload only in its directional network pipe. Core does not
place received application payload in a hidden PAIR queue and does not copy
reply payload into a completion deque. The remaining terminal-callback metadata
queue contains only payloadless timeout, disconnect, and shutdown outcomes; it
is not a transport lane or wire record.

### Transport-liveness boundary

TCP and WebSocket engines deliver orderly disconnects, read/write failures, and
protocol failures to their sessions. A session reports these conditions through
the socket monitor and updates reconnect state for configured endpoints.
Operating-system TCP keepalive and the TCP retransmission limit remain transport
options; an engine creates no separate application control frame for them.

Framework service-protocol liveness messages travel as raw application payload.
Core does not interpret their body or deadline. Each language Framework runtime
handles them through its infrastructure queue and scheduler.

The Core source boundary contains no `ZLINK_OPT_HEARTBEAT_IVL`,
`ZLINK_OPT_HEARTBEAT_TTL`, `ZLINK_OPT_HEARTBEAT_TIMEOUT`,
`zmp_control_heartbeat`, `zmp_control_heartbeat_ack`, or codec, parser, and engine
state that handles those values. It also contains no `heartbeat_ivl_timer_id`,
`heartbeat_ttl_timer_id`, `heartbeat_timeout_timer_id`, or corresponding
callback branches. Generic engine timers and reconnect timers are raw-transport
resources.

### Timer boundary

Core timers are monotonic scheduling primitives for raw-socket engines,
reconnect, and poller integration. They know nothing about application Spot
turns, Actor lifecycles, Instance leases, or transfer phases. Framework object
timers and deadline schedulers use the binding's public timer or poller API, or
the scheduler provided by the language runtime.

Closing a timer owner cancels callback registration and prevents a pending
callback from referring to owner state again. A timer ID has meaning only
within one owner lifetime and is not a Framework operation ID or generation.

### Monitor boundary

Core monitors report raw bind, accept, connect, disconnect, retry, protocol,
and transport failures. An event describes a raw socket and connection lifetime;
it does not contain MeshName, ChannelName, peer admission, Spot, Actor, Location
Store, or host termination results.

Framework consumes raw events through the binding's public monitor API and
applies them to its own peer registry and state reducer. The Core monitor queue
and Framework typed observer queues are separate resources. Core monitoring
does not implement slow-observer handling, coalescing, or metric policy.

### Raw-only invariants

- Core source and public ABI own no service protocol command or state machine.
- Core creates no application mailbox, ready owner, claim, reply token, or
  terminal request state.
- Core does not interpret Spot, Actor, or Instance identity, generation, or
  activation barriers.
- Core does not call a Location Store, Checkpoint Store, lease, owner CAS, or
  maintenance recovery operation.
- Raw engine timers and raw monitors are connection resources.
- Framework uses only public binding APIs and does not depend on private Core
  symbols.
- Raw-socket options and monitor events are not exposed unchanged as Framework
  service APIs.
