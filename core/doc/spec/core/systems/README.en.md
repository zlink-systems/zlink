[한국어](README.ko.md) | English

<!-- zlink-nav:start -->
[Core Spec Index](../README.en.md) | [Previous: RAW (STREAM) Protocol Details](../protocol/02-raw.en.md) | [Next: Architecture](01-architecture.en.md)
<!-- zlink-nav:end -->

# Systems -- Index

This document links Core internal-structure documents that cut across
multiple socket types and common contracts. Each document is an
implementation description; the public contract it supports is owned by the
related spec document it links to.

| Document | Content |
|---|---|
| [Architecture](01-architecture.en.md) | Internal components and boundaries from the public API to the I/O threads |
| [Threading Model](02-threading-model.en.md) | Thread kinds and responsibilities |
| [I/O Thread](03-io-thread.en.md) | How I/O threads are created and how work is distributed |
| [Thread Safety](04-thread-safety.en.md) | Implementation of the three-tier thread-safety contract |
| [Per-Connection Memory](05-connection-memory.en.md) | Memory components allocated per connection |
| [Auto HWM Internals](06-auto-hwm.en.md) | Internal Auto HWM state and calculation |
| [Core Source Layout](07-core-source-layout.en.md) | Source directory layout and include direction |
| [Core POSD Module Structure](08-posd-module-structure.en.md) | Responsibility separation across layers |
| [Core Design Decisions](09-design-decisions.en.md) | Key design decisions |
