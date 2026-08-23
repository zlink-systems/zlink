[한국어](README.ko.md) | English

<!-- zlink-nav:start -->
[Specification Index](../README.en.md) | [Next: Public Contract Governance](00-public-contract-governance.en.md)
<!-- zlink-nav:end -->

# ZLink Core specification

This index links the Core public C ABI contract exposed by `zlink.h`. Formal API documents describe only public contracts; they do not describe source directories, socket wiring, or queue structure. A contract document covers its related internal structure in its own "Internals" section, and cross-cutting topics live in the systems documents.

## 1. Common contracts

| Document | Content |
|---|---|
| [Public-contract governance](00-public-contract-governance.en.md) | Consistency among specification, headers, tests, and packages |
| [Context](01-context.en.md) | Context creation, shutdown, and configuration |
| [Message](02-message.en.md) | Message lifecycle, routing IDs, ownership, and multipart internals |
| [Errors](03-errors.en.md) | Public result enums, errno, version, and the result-errno mapping |
| [Events](04-events.en.md) | Common event types and readiness meaning |
| [Polling](05-polling.en.md) | Poll items, pollers, and source support |
| [Monitoring](06-monitoring.en.md) | Raw-socket monitors and status snapshots |
| [Utilities](07-utilities.en.md) | Timers, threads, stopwatch, and atomic helpers |
| [Runtime boundary](08-runtime-boundary.en.md) | Core raw C ABI and Framework service responsibility boundary, and internal layering |

## 2. Socket contracts

| Document | Content |
|---|---|
| [Socket index](socket/README.en.md) | Common lifecycle, options, send, receive, and internal option defaults |
| [PAIR](socket/01-pair.en.md) | One-to-one connection |
| [PUB](socket/02-pub.en.md) | Classic fanout publisher |
| [SUB](socket/03-sub.en.md) | Classic fanout subscriber |
| [XPUB](socket/04-xpub.en.md) | Subscription-aware publisher |
| [XSUB](socket/05-xsub.en.md) | Upstream subscription socket |
| [DEALER](socket/06-dealer.en.md) | Asynchronous request source |
| [ROUTER](socket/07-router.en.md) | Raw routing-ID router |
| [STREAM](socket/08-stream.en.md) | Raw TCP or WebSocket session socket, with WS/WSS internals |

## 3. Wire protocols

| Document | Content |
|---|---|
| [Protocol index](protocol/README.en.md) | Index of the ZMP and RAW protocol documents |
| [ZMP Protocol Details](protocol/01-zmp.en.md) | The ZMP v1.0 wire protocol |
| [RAW (STREAM) Protocol Details](protocol/02-raw.en.md) | The RAW wire protocol used by STREAM |

## 4. Cross-cutting system structure

| Document | Content |
|---|---|
| [Systems index](systems/README.en.md) | Index of Core internal system documents |
| [Architecture](systems/01-architecture.en.md) | Internal components from the public API to the I/O threads |
| [Threading Model](systems/02-threading-model.en.md) | Thread kinds and responsibilities |
| [I/O Thread](systems/03-io-thread.en.md) | How I/O threads are created and how work is distributed |
| [Thread Safety](systems/04-thread-safety.en.md) | Implementation of the three-tier thread-safety contract |
| [Per-Connection Memory](systems/05-connection-memory.en.md) | Memory components allocated per connection |
| [Auto HWM Internals](systems/06-auto-hwm.en.md) | Internal Auto HWM state and calculation |
| [Core Source Layout](systems/07-core-source-layout.en.md) | Source directory layout and include direction |
| [Core POSD Module Structure](systems/08-posd-module-structure.en.md) | Responsibility separation across layers |
| [Core Design Decisions](systems/09-design-decisions.en.md) | Key design decisions |
