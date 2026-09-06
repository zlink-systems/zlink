---
title: "Thread safety"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/04-thread-safety/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: I/O Thread](03-io-thread.en.md) | [Next: Per-Connection Memory](05-connection-memory.en.md)
<!-- zlink-nav:end -->

# Thread safety

> **What this chapter defines** — an implementation description of how Core enforces which APIs
> may be called concurrently from multiple threads and which APIs must be serialized.

## 1. Thread safety overview

zlink's public APIs assume that multiple application threads may call the same handle concurrently.
The public contract defines which calls may run concurrently and which calls must be serialized. This
document describes how Core enforces that contract internally. Its intended audience is Core
maintainers.

This document describes the implementation. The following documents, rather than this one, own the
thread-safety contracts on which callers rely.

| Related contract | Defining document |
|---|---|
| Concurrent-call scope and close rules for [socket](../glossary.en.md#socket) handle APIs | [Socket Common §2 Thread safety](../socket/README.en.md#2-thread-safety) |
| Whether each function may be called concurrently | The "**Thread safety:**" label in each spec document's function reference |
| Thread types and responsibilities | [Threading model](02-threading-model.en.md) |

## 2. Three tiers

The Core implementation enforces three tiers of public APIs based on their concurrent-call
characteristics.

| Tier | Meaning |
|---|---|
| Hot path | Multiple application threads may concurrently call supported socket send and request operations. |
| Control path | Options, bind and connect operations, and handler registration are serialized per handle. |
| Lifecycle | Close and destroy operations do not run concurrently with another mutable operation on the same handle. |

## 3. Internal rules

Core enforces the three tiers through the following rules.

**Send and receive paths.** The socket semantic layer, which implements the send and receive behavior
for each socket type, protects the routing state used for peer selection with only the locks required.
Admission of a message into a pipe—the queue that transfers messages between a socket and a
session/engine (see [Architecture](01-architecture.en.md))—commits together with the transfer of
message ownership. In other words, the pipe's decision to accept a message and the transfer of that
message's ownership to the library do not occur separately. The [I/O thread](../glossary.en.md#io-thread)
for a connection owns the state of the engine that handles protocol processing for that connection.

**Receive modes.** Callback mode and synchronous receive mode are single consumers of the same
queue. Because only one consumer may remove messages from a queue, the two modes cannot be registered
at the same time.

**Public API guard and close.** The guard at the public API boundary manages only the handle pin—a
marker that keeps the handle valid while an API call is in progress—and the close state. It does not
branch on service kind or application lifecycle. The guard never waits: a new entry after close is
accepted, and a close while an API call is in flight, are both rejected immediately, and the result the
caller observes (`ESHUTDOWN`, `EBUSY`) is defined by
[Socket Common §2 Thread safety](../socket/README.en.md#2-thread-safety).
