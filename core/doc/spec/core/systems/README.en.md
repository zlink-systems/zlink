[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/) | English

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
| [01. Architecture](01-architecture.en.md) | Internal components and boundaries from the public API to the I/O threads |
| [02. Threading Model](02-threading-model.en.md) | Thread kinds and responsibilities |
| [03. I/O Thread](03-io-thread.en.md) | How I/O threads are created and how work is distributed |
| [04. Thread Safety](04-thread-safety.en.md) | Implementation of the three-tier thread-safety contract |
| [05. Per-Connection Memory](05-connection-memory.en.md) | Memory components allocated per connection |
| [06. Auto HWM Internals](06-auto-hwm.en.md) | Internal Auto HWM state and calculation |
| [07. Core Source Layout](07-core-source-layout.en.md) | Source directory layout and include direction |
| [08. Core POSD Module Structure](08-posd-module-structure.en.md) | Responsibility separation across layers |
| [09. Core Design Decisions](09-design-decisions.en.md) | Key design decisions |
