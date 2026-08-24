---
title: "Systems"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/) | English

<!-- zlink-nav:start -->
[Core Spec Index](../README.en.md) | [Previous: RAW (STREAM) Protocol Details](../protocol/02-raw.en.md) | [Next: Architecture](01-architecture.en.md)
<!-- zlink-nav:end -->

# Systems

> **What this chapter defines** — an index of cross-cutting Core internal
> structure documents that do not belong to one specific contract document.

This document links Core internal-structure documents that span multiple
socket types and common contracts. Most of what each document describes is
implementation detail, and the public contract supported by that content is
owned by the related specification document to which it links. As an
exception, [06. Auto HWM](06-auto-hwm.en.md) owns the Auto HWM budget contract
together with its internal implementation.

| Document | Content |
|---|---|
| [01. Core runtime architecture](01-architecture.en.md) | Internal components and boundaries from the public API to the I/O thread |
| [02. Core threading model](02-threading-model.en.md) | Thread types and responsibilities |
| [03. I/O Thread](03-io-thread.en.md) | How I/O threads are created and how work is distributed |
| [04. Thread Safety](04-thread-safety.en.md) | Implementation of the three-tier thread-safety contract |
| [05. Connection Memory](05-connection-memory.en.md) | Memory components for each connection |
| [06. Auto HWM](06-auto-hwm.en.md) | Auto HWM budget contract and internal structure |
| [07. Core Source Layout](07-core-source-layout.en.md) | Source directory layout and include direction |
| [08. Core POSD Module Structure](08-posd-module-structure.en.md) | Responsibility separation across layers |
| [09. Core Design Decisions](09-design-decisions.en.md) | Key design decisions |
