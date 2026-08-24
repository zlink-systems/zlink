---
title: "ZLink Core Specification"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/) | English

<!-- zlink-nav:start -->
[Specification Index](../README.en.md) | [Next: Public Contract Governance](00-public-contract-governance.en.md)
<!-- zlink-nav:end -->

# ZLink Core Specification

> **What this chapter defines** — The index of the complete Core C API specification and the scope covered by each chapter.

This index links the Core public C ABI contract documentation exposed by `zlink.h`. Each contract document describes only public contracts; it does not define internals such as source directories, socket wiring, or queue structure as contracts. Internal structures related to a contract are covered in the "Internals" section of the same document, while cross-cutting topics spanning multiple documents are covered in the systems documents.

## 1. Reading Order

The following order is recommended for first-time readers. Each chapter can be read independently, but this order follows the direction in which later chapters build on concepts introduced earlier.

1. [Context](01-context.en.md) — Start with the top-level container to which every socket belongs.
2. [Message](02-message.en.md) — The unit of data exchanged by sockets and its ownership rules.
3. [Socket Common Contracts](socket/README.en.md) — Lifecycle, options, and send and receive forms common to all socket types.
4. Individual socket documents — Read only the chapter for the socket type you use
   (from [PAIR](socket/01-pair.en.md) through [STREAM](socket/08-stream.en.md)).
5. [Errors](03-errors.en.md) — Finish with the result and errno system shared by all functions.

For wire protocols, events, polling, monitoring, and internal system structure, use the [task-based index](#6-task-based-index) as needed.

## 2. Common Contracts

| Document | Content |
|---|---|
| [Public Contract Governance](00-public-contract-governance.en.md) | Procedure for keeping specifications, headers, tests, and packages consistent |
| [Context](01-context.en.md) | Context creation, shutdown, and configuration |
| [Message](02-message.en.md) | Message lifecycle, routing IDs, ownership, and multipart internals |
| [Errors](03-errors.en.md) | Public result enums, errno, version, and the result-errno mapping |
| [Events](04-events.en.md) | Common event types and readiness semantics |
| [Polling](05-polling.en.md) | Poll items, pollers, and the source support matrix |
| [Monitoring](06-monitoring.en.md) | Raw socket monitors and status snapshots |
| [Utilities](07-utilities.en.md) | Timers, threads, stopwatches, and atomic helpers |
| [Runtime Boundary](08-runtime-boundary.en.md) | Responsibility boundary between the Core raw C ABI and Framework services, and the internal layer structure |

## 3. Socket Contracts

| Document | Content |
|---|---|
| [Socket Common Contracts](socket/README.en.md) | Common lifecycle, options, send and receive operations, and internal option defaults |
| [Socket — PAIR](socket/01-pair.en.md) | One-to-one connection |
| [Socket — PUB](socket/02-pub.en.md) | Classic fanout publisher |
| [Socket — SUB](socket/03-sub.en.md) | Classic fanout subscriber |
| [Socket — XPUB](socket/04-xpub.en.md) | Subscription-aware publisher |
| [Socket — XSUB](socket/05-xsub.en.md) | Upstream subscription socket |
| [Socket — DEALER](socket/06-dealer.en.md) | Asynchronous request socket |
| [Socket — ROUTER](socket/07-router.en.md) | Routing-ID-based raw router |
| [Socket — STREAM](socket/08-stream.en.md) | Raw TCP/WS session socket and internal WS/WSS optimizations |

## 4. Wire Protocols

| Document | Content |
|---|---|
| [Protocol](protocol/README.en.md) | Index of the ZMP and RAW protocol documents |
| [Protocol — ZMP v1.0](protocol/01-zmp.en.md) | ZMP v1.0 wire protocol |
| [Protocol — RAW](protocol/02-raw.en.md) | RAW wire protocol used by STREAM |

## 5. Cross-Cutting System Structure

| Document | Content |
|---|---|
| [Systems](systems/README.en.md) | Index of Core internal system documents |
| [Core Runtime Architecture](systems/01-architecture.en.md) | Internal components from the public API to the I/O threads |
| [Core Threading Model](systems/02-threading-model.en.md) | Thread types and responsibilities |
| [I/O Thread](systems/03-io-thread.en.md) | I/O thread creation and work distribution |
| [Thread Safety](systems/04-thread-safety.en.md) | Implementation of the three-tier thread-safety contract |
| [Connection Memory](systems/05-connection-memory.en.md) | Per-connection memory components |
| [Auto HWM](systems/06-auto-hwm.en.md) | Auto HWM budget contract and internal structure |
| [Core Source Layout](systems/07-core-source-layout.en.md) | Source directory structure and include direction |
| [Core POSD Module Structure](systems/08-posd-module-structure.en.md) | Separation of responsibilities by layer |
| [Core Design Decisions](systems/09-design-decisions.en.md) | Key design decisions |

## 6. Task-Based Index

If you know the task you need to perform, use the table below to find the starting document. The linked document owns the contract it describes.

| Task | Start Here |
|---|---|
| Implement a language binding | Read the complete [Reading Order](#1-reading-order), then [Events](04-events.en.md), [Polling](05-polling.en.md), [Monitoring](06-monitoring.en.md), and [Utilities](07-utilities.en.md) |
| Implement error handling | [Errors](03-errors.en.md) and the errors section of the socket chapter you use |
| Wait for socket readiness or monitor multiple sockets | [Events](04-events.en.md), [Polling](05-polling.en.md), and [Monitoring](06-monitoring.en.md) |
| Interoperate with another language or implementation at the wire level | [Protocol — ZMP v1.0](protocol/01-zmp.en.md) and [Protocol — RAW](protocol/02-raw.en.md) |
| Understand queue sizing and memory use | [Auto HWM](systems/06-auto-hwm.en.md) and [Connection Memory](systems/05-connection-memory.en.md) |
| Maintain Core internals | Start with [Systems](systems/README.en.md) and read in order |
| Change specifications, headers, and tests together | [Public Contract Governance](00-public-contract-governance.en.md) |
| Confirm the boundary for adding a higher layer on top of Core | [Runtime Boundary](08-runtime-boundary.en.md) |
