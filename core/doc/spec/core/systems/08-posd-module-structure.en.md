---
title: "Core POSD module structure"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/08-posd-module-structure/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Core Source Layout](07-core-source-layout.en.md) | [Next: Design Decisions](09-design-decisions.en.md)
<!-- zlink-nav:end -->

# Core POSD module structure

> **What this chapter defines** — the criteria for organizing Core source into layers and modules
> according to POSD principles, and the responsibility boundary of each layer.

## 1. Goal

The design principle of creating deep modules that hide implementation complexity behind narrow
public interfaces is called POSD (*A Philosophy of Software Design*). Core follows this principle
by hiding transport, connection, pipe, and protocol complexity behind a narrow raw
[socket](../glossary.en.md#socket) (an endpoint that sends and receives messages) C ABI.
Core does not re-expose application service semantics as Core helpers or options.

> **Contract ownership** — This entire document is an implementation description, not a contract
> definition. If the layer structure described here differs from the code, update this document to
> match the code. The documents in the following table own the public behavior that Core guarantees
> to applications; this document does not redefine those contracts.

| Related contract | Defining document |
|---|---|
| Directory layout and include direction | [Core source layout](07-core-source-layout.en.md) |
| Context creation, options, and termination | [Context](../01-context.en.md) |
| Message lifecycle and ownership | [Message](../02-message.en.md) |
| Socket creation, options, send, and receive | [Socket common specification](../socket/README.en.md) |

## 2. Responsibilities by layer

Core source is divided into the following five layers. Each layer owns only the responsibilities in
its row.

| Layer | Responsibility |
|---|---|
| Public C API | Validates arguments, manages handles, expresses ownership, and maps internal results to C API results |
| Socket semantics | Decides routing per socket type, handles multipart behavior, and manages request correlation |
| Runtime core | Manages contexts, sessions, pipes, and mailbox commands, and drives their lifecycle |
| Engine | Performs ZMP and RAW framing and handshake |
| Transport | Performs I/O over TCP, WebSocket, IPC, inproc, and TLS |

The boundaries apply in both directions. The public API does not branch directly on transport types
or protocol parsers. The runtime core does not know policies specific to socket types, and engines do
not interpret application payload semantics. [Core source layout](07-core-source-layout.en.md)
describes the directory for each layer and the restrictions on include direction.

## 3. Warning signs

The following signs indicate that the layer boundaries above are breaking down. Framework is a
higher layer that provides application services on top of Core.

- A Framework service concept is added as a public Core type or option.
- The API facade becomes a service-specific pass-through that forwards arguments unchanged.
- The same protocol field is defined manually in multiple engines or bindings.
- Socket semantics are pushed into an application callback or Framework helper.
- Framework directly uses a private Core header or symbol.

When one of these signs appears, review the responsibility boundary before adding a new helper.
