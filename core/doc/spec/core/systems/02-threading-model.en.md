---
title: "Core threading model"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/02-threading-model/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Architecture](01-architecture.en.md) | [Next: I/O Thread](03-io-thread.en.md)
<!-- zlink-nav:end -->

# Core threading model

> **What this chapter defines** — The types of threads that Core actually creates, their
> responsibilities, and the communication structure between them.

## 1. Threading model overview

zlink Core uses several types of threads with distinct roles, centered on
[Context](../glossary.en.md#context), the top-level container that holds I/O processing threads and
[sockets](../glossary.en.md#socket). This document explains those thread types, their
responsibilities, and how commands and payloads move between threads. It is intended for developers
who maintain Core internals or review thread boundaries.

Most of this chapter describes the implementation. When the implementation changes, update this
document to match the code. The following documents own the public contracts that callers depend on;
this chapter does not redefine those contracts.

| Related contract | Defining document |
|---|---|
| Public boundaries between threads | [Core runtime boundary](../08-runtime-boundary.en.md) |
| Thread safety of each socket API | [Common socket thread safety](../socket/README.en.md#2-thread-safety) |
| Context options such as the number of I/O threads | [Context](../01-context.en.md#4-options) |

## 2. Thread types

The background threads that perform network sends and receives are called
[I/O threads](../glossary.en.md#io-thread). Four types of threads participate in the Core runtime.

| Thread | Responsibility | Count |
|---|---|---|
| Application thread | Public API calls and blocking waits | Application-owned |
| I/O thread | Transport completions, engine state, and socket callbacks | Context `io_threads` |
| Reaper thread | Cleanup of terminated sockets and owned objects | One per Context |
| Timer scheduler | Generic timer deadlines and fire counts | Runtime-owned |

In the table, an engine is an internal object responsible for transport sends and receives and for
protocol encoding and decoding for one connection. [I/O thread internals](03-io-thread.en.md)
describes the I/O thread event loop, engine processing, and connection assignment criteria.

Core 0.13.0 has no service mailbox or MeshNode-specific ingress thread.

## 3. Cross-thread communication

Commands from an application thread are delivered to the owner thread through a mailbox, the channel
used to pass commands between threads. Payload data moves between the socket semantic layer and an
engine through pipe queues. Each connection is pinned to one I/O thread, and multiple I/O threads do
not mutate the same connection's engine state concurrently.

The following paragraph summarizes the contract owned by
[common socket thread safety](../socket/README.en.md#2-thread-safety). Send, publish, and routed send
on supported handle types can be used concurrently from multiple threads. Low-frequency control paths
are serialized for correctness. Unless a formal API specifies a different contract, receive is
single-consumer: only one consumer thread receives at a time.

## 4. Callbacks

Socket message and transport monitor callbacks run on the thread specified by their formal API.
Reentering a destructive close or receive-mode change on the same handle from within a callback is
rejected with the formal result and errno. No new callback starts after Context termination.
