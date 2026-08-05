[한국어](09-runtime-boundary.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md)

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
