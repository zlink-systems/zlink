---
title: "Core runtime architecture"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/01-architecture/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Systems Overview](README.en.md) | [Next: Threading Model](02-threading-model.en.md)
<!-- zlink-nav:end -->

# Core runtime architecture

> **What this chapter defines** — the internal components and boundaries that
> messages traverse from the public C API to the I/O threads.

## 1. Core runtime overview

Core is a raw socket runtime. The public C API enters the implementation of a
[socket](../glossary.en.md#socket) — an endpoint that exchanges messages — and
the socket implementation exchanges commands and messages with
[I/O thread](../glossary.en.md#io-thread) objects — background threads that
perform the actual network send and receive operations — through mailboxes that
carry commands across thread boundaries and pipes that carry messages.

The components are stacked in the following layers.

```text
+------------------------------------------------------+
| Public C API                                         |
+------------------------------------------------------+
| Socket patterns and eventing                         |
+------------------------------------------------------+
| Context, socket base, sessions, pipes, mailboxes     |
+------------------------------------------------------+
| ZMP or raw engines                                   |
+------------------------------------------------------+
| TCP, IPC, inproc, WebSocket, TLS transports          |
+------------------------------------------------------+
```

This document describes only the internal structure of these components
(implementation description — when the implementation changes, update this
document to match the code). Its intended audience is Core maintainers. The
following documents, rather than this document, own the public contracts that
each component exposes.

| Related contract | Defining document |
|---|---|
| Responsibility boundary that keeps Core a raw socket runtime only | [Runtime Boundary](../08-runtime-boundary.en.md) |
| Context lifetime and options | [Context](../01-context.en.md) |
| Socket creation, options, sending, and receiving | [Socket Common](../socket/README.en.md) |
| Socket monitor contract | [Monitoring](../06-monitoring.en.md) |
| Utilities such as pollers and timers | [Utilities](../07-utilities.en.md) |

## 2. Context and threads

[Context](../glossary.en.md#context) is the top-level container for I/O threads
and sockets. Its implementation, `ctx_t`, owns socket slots, I/O threads, a
reaper thread dedicated to the final cleanup of closed sockets, the endpoint
registry, and the generic control runtime used for monitor and timer callbacks.

Socket closure sends a termination command and waits until the owned objects
release their resources.

## 3. Socket and session path

`socket_base_t` owns public socket state and pattern-specific behavior. A
session is an internal object that represents one transport connection. A pipe
carries messages between the socket thread and session or engine objects while
preserving multipart order.

## 4. Engines and transports

The Asio engine submits asynchronous reads and writes and receives completion
callbacks. The ZMP engine encodes zlink message frames, while the raw engine
passes byte-stream payloads. Transport classes handle endpoint parsing,
connection establishment, and operating-system I/O.

## 5. Eventing

The poller combines readiness for sockets, file descriptors, and generic
timers in one place. The socket monitor observes raw transport and protocol
transitions. The control runtime executes monitor and timer callbacks outside
the socket I/O threads.
