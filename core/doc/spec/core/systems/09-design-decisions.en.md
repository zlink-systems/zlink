---
title: "Core design decisions"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/09-design-decisions/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: POSD Module Structure](08-posd-module-structure.en.md) | [Next: Core Hot Path](10-hot-path.en.md)
<!-- zlink-nav:end -->

# Core design decisions

> **What this chapter defines** — The Core's key design decisions, such as asynchronous I/O and the shape of the socket API, and the reasons behind them.

## 1. Design decision overview

This document records the Core's key design decisions and their rationale. The document that owns each
contract, rather than this document, defines the exact behavior that each decision guarantees to applications—this
document records only what was decided and why.

| Decision | Document that owns the contract and details |
|---|---|
| [Asynchronous I/O](#2-asynchronous-io) | [Core runtime architecture](01-architecture.en.md) |
| [Message ownership](#3-message-ownership) | [Message](../02-message.en.md) |
| [Multipart atomicity](#4-multipart-atomicity) | [Message](../02-message.en.md#4-multipart) |
| [Typed socket surface](#5-typed-socket-surface) | [Socket common specification](../socket/README.en.md) |
| [Eventing separation](#6-eventing-separation) | [Events](../04-events.en.md), [Polling](../05-polling.en.md), [Monitoring](../06-monitoring.en.md) |

## 2. Asynchronous I/O

The Core's asynchronous I/O is built on Boost.Asio. Boost.Asio provides a single completion-based I/O
model across supported network transports: I/O operations are requested first, and their completion is
reported later.

The engine keeps protocol parsing and transport operations behind the session interface. A session is an
internal object that represents one transport connection. [Core runtime architecture](01-architecture.en.md)
describes the internal structure of the engine and sessions.

## 3. Message ownership

Small payloads are stored inline in the message structure, while large payloads use shared storage referenced
by multiple message handles. Explicit move, copy, and close operations express ownership across the C API
boundary without exposing allocator selection.

## 4. Multipart atomicity

A multipart send sequence that groups multiple frames (parts) into one logical message remains one logical
queue operation. The send code for each [socket](../glossary.en.md#socket) type delegates transaction handling
to the common multipart path, so callers do not have to duplicate partial-failure cleanup for disposing of the
remaining parts when a sequence fails midway.

## 5. Typed socket surface

Pattern-specific metadata is returned through typed APIs. Routing IDs (byte sequences by which a ROUTER socket
identifies a specific peer), topics, request sequences, and subscription state are not inserted into application
payload frames.

## 6. Eventing separation

The poller reports readiness—the state in which it is worthwhile for a source to proceed with receive or send—
the monitor reports transport/protocol transitions, and the generic timer reports time events. These three
mechanisms do not interpret application payloads.
