---
title: "Core Glossary"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/glossary/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md)
<!-- zlink-nav:end -->

# Core Glossary

> **What this chapter defines** — concise definitions of the key terms shared throughout the Core
> specification. The exact contract for each term is owned by its corresponding spec document;
> other documents link here when they first use a term.

### Context

zlink's top-level container for I/O threads and sockets. The exact contract is owned by
[Context](01-context.en.md).

### I/O thread

A background thread that Context creates and manages. It performs the actual network send and receive operations.

### socket

An endpoint that exchanges messages. It must belong to a Context. The contract is owned by
[Socket Common](socket/README.en.md).

### HWM

High-Water Mark. A value that limits the bytes retained in a queue to apply backpressure.

### backpressure

The behavior that limits further submissions by a sender when downstream cannot keep up with the processing rate.

### Auto HWM budget

The total number of bytes that Core calculates from memory input and uses as the basis for allocating HWM among
application queues. The contract is owned by [Auto HWM](systems/06-auto-hwm.en.md).

### directional queue

A physical queue that holds messages for one application direction. It is counted only once even when both
endpoints observe the same direction.

### generation

A version number that distinguishes a recreated queue in the same direction from its predecessor.

### effective cap

The upper bound applied to the Auto HWM budget. It is the greater of the profile's fixed cap and the sum of the
minimums for active queues.

### water-filling

A distribution method that allocates the remaining budget evenly among queues that have not yet reached their
caps, like filling them with water.

### completion progress lane

A separate physical connection that ROUTER-ROUTER uses to break a bidirectional request wait cycle. It
carries terminal replies, error replies, and receive-flow control and is excluded from HWM admission and
Application budget calculation. DEALER-ROUTER has no such lane; replies use its single Application
connection. This physical connection is distinct from the socket-local completion queue consumed by
`zlink_completion_recv()`.
