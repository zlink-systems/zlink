[한국어](runtime-boundary.ko.md) | English

# Core Raw-Runtime Internal Boundary

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

## Public binding boundary

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

## Socket and pipe ownership

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
reply payload into a completion deque. The remaining completion control queue
contains only callback metadata for payloadless terminal outcomes.

## Transport-liveness boundary

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

## Timer boundary

Core timers are monotonic scheduling primitives for raw-socket engines,
reconnect, and poller integration. They know nothing about application Spot
turns, Actor lifecycles, Instance leases, or transfer phases. Framework object
timers and deadline schedulers use the binding's public timer or poller API, or
the scheduler provided by the language runtime.

Closing a timer owner cancels callback registration and prevents a pending
callback from referring to owner state again. A timer ID has meaning only
within one owner lifetime and is not a Framework operation ID or generation.

## Monitor boundary

Core monitors report raw bind, accept, connect, disconnect, retry, protocol,
and transport failures. An event describes a raw socket and connection lifetime;
it does not contain MeshName, ChannelName, peer admission, Spot, Actor, Location
Store, or host termination results.

Framework consumes raw events through the binding's public monitor API and
applies them to its own peer registry and state reducer. The Core monitor queue
and Framework typed observer queues are separate resources. Core monitoring
does not implement slow-observer handling, coalescing, or metric policy.

## Raw-only invariants

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
