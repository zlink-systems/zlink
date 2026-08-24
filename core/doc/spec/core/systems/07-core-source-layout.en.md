---
title: "Core source layout"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/07-core-source-layout/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Auto HWM](06-auto-hwm.en.md) | [Next: POSD Module Structure](08-posd-module-structure.en.md)
<!-- zlink-nav:end -->

# Core Source Layout

> **What this chapter defines** — an implementation description of how the raw runtime source
> remaining in Core 0.13.0 is organized into directories and which include directions are allowed.

## 1. Core Source Layout Overview

This document explains the responsibilities of each directory containing the raw runtime source
that remains in Core 0.13.0 and the allowed include directions between those directories. Its
intended audience is maintainers who modify Core source or decide where to place new files.

This entire document is an **implementation description**, not a public contract. If the document
and the code disagree, the code is authoritative, and the document is updated to match the code.
The public contracts on which applications depend are owned by the specification document for each
feature, not by this document.

The documents that own the related contracts are as follows.

| Related contract | Defining document |
|---|---|
| Installed headers and export manifest | [Core Runtime Boundary](../08-runtime-boundary.en.md) |
| Concise definitions of each term | [Core Glossary](../glossary.en.md) |

## 2. Layer Model

The source is divided into the following areas.

```text
core/
|-- include/                public raw C ABI (root zlink.h, zlink_enum.h, zlink_errno.h)
|   `-- zlink/              domain headers (common.h, core/, message/, socket/, eventing/)
|-- src/api/                validation and C ABI facade (core, message, monitoring, socket)
|-- src/runtime/sockets/    socket-type semantics
|-- src/runtime/core/       context, session, pipe, and message runtime; poller, timer, and monitor foundations
|-- src/runtime/engine/     ZMP and RAW engines
|-- src/runtime/protocol/   ZMP and RAW wire codecs (encoder/decoder)
|-- src/runtime/transports/ transport integration
|-- src/runtime/utils/      shared utilities such as allocator, clock, and err
`-- tests/                  raw contract and integration tests
```

In these areas, a [socket](../glossary.en.md#socket) is an endpoint that exchanges messages, and a
[Context](../glossary.en.md#context) is the top-level container for I/O threads and sockets.
`src/runtime/sockets/` implements the semantics of each socket type, while `src/runtime/core/`
implements the context, session, pipe, and message runtime.

## 3. Include Direction

The public API facade in `src/api/` calls the socket-semantics layer, and the socket-semantics layer
uses the runtime core's shared connection and pipe mechanisms. Engines and transports do not know
the application meaning of socket types. Lower layers do not include headers from higher layers.

Core source does not contain MeshName, ChannelName, application mailboxes, Spot, Actor, Location
Store, or service lifecycle state. It also does not add a separate native seam for Framework
convenience.

## 4. Public Include Rules

The root `zlink.h` includes only `zlink/common.h` and the domain headers for core
(`zlink/core/api.h`), message, socket, and eventing.
Public headers do not include private headers from `src/`. Installed headers and the export manifest
must match the [Core Runtime Boundary](../08-runtime-boundary.en.md).
