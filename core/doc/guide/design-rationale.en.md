[한국어](design-rationale.ko.md)

# Design Rationale — Why It Was Built This Way

> **What this chapter answers** — it explains the reasoning behind zlink's
> core design decisions from a user's perspective. Implementation detail is
> owned by [internals](../internals/architecture.en.md).

This document explains, from a user's perspective, the **reasoning** behind
the core design decisions zlink adopted. Implementation detail is owned by
[internals](../internals/architecture.en.md); this document instead focuses
on "what this choice means for the user." It's written for a reader
evaluating whether to adopt zlink or trying to understand its performance
characteristics.

## Starting Point — What Changed From libzmq

zlink starts from [libzmq](https://github.com/zeromq/libzmq) v4.3.5 and is a
library that **focuses on the core patterns and narrows the surface**. See
[the overview](01-overview.en.md) for the detailed comparison table. The
gist is:

- 17 socket types → **8** (PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM).
- The I/O engine went from a self-built poll/epoll/kqueue to **Boost.Asio**
  (bundled, no external dependency).
- Encryption went from CURVE (libsodium) to **TLS** (OpenSSL).
- The dependency footprint narrowed to **just OpenSSL**.

The reason for narrowing is simple. Exposing less makes it easier to
**polish each pattern deeply and keep it consistent**, and gives the user
fewer choices, which also means less room to choose wrong.

## Core Design Principles

### Zero-Copy — Small Messages Inline, Large Messages Reference-Counted

A small message (41 bytes or less, on 64-bit) is stored directly inside the
message object without a separate heap allocation (VSM, Very Small
Message). A message larger than that is shared without copying, through
reference counting.

**What this means for the user**: in a workload with many small control
messages (ticks, heartbeats, short commands), allocation and copy cost
disappears. The move semantics where a send "consumes" the message also
come from this model — copy explicitly when you need to keep it
([09 Message API](09-message-api.en.md)).

> VSM is **only a memory optimization — it has no effect on the wire
> format.** The receiving side never needs to know whether the sending side
> used inline storage ([ZMP reference](zmp-protocol.en.md)).

### Lock-Free — YPipe For Inter-Thread Communication

Message delivery between threads uses a CAS (Compare-And-Swap)-based FIFO
queue (YPipe) instead of a lock.

**What this means for the user**: there's no lock contention on the hot
path, so multicore scaling works well. In exchange, a socket is not
thread-safe — the premise is that the same socket is never handled from
multiple threads at once ([11 Thread Safety](11-thread-safety.en.md)).

### True Async — The Proactor Pattern

Built on Boost.Asio, I/O **completion** events are delivered to a handler
(Proactor). This is a structure that gets notified of completion rather
than polling I/O directly.

**What this means for the user**: a callback runs on the I/O thread the
Context owns — keep a callback short, don't hold a lock inside it, and
don't close a handle from inside it. Multiple sockets are grouped under one
loop with a poller (the concept is in [02 Core API](02-core-api.en.md); the
per-language surface is in each
[binding guide](../../../bindings/doc/guide/README.ko.md)).

### Protocol Agnostic — Separating Transport From Protocol

The wire protocol (ZMP) and the transport (tcp/ipc/inproc/ws/tls) are
clearly separated.

**What this means for the user**: the same messaging code moves from
inproc (same process) to tcp (network) by changing only the transport —
just the address scheme changes. For tls/wss, though, beyond the address
scheme, you also need to configure `zlink_set_tls_server()` (server
cert/key) and, if needed, `zlink_set_tls_client()`
([04 Transport](04-transports.en.md),
[05 TLS/Security](05-tls-security.en.md)).

## Going Deeper

- The full layered architecture: [internals/architecture](../internals/architecture.en.md)
- Trade-offs behind design decisions: [internals/design-decisions](../internals/design-decisions.en.md)
- The wire protocol: [ZMP protocol reference](zmp-protocol.en.md)
- How far the delivery guarantee goes: [Reliability and delivery guarantees](reliability.en.md)
