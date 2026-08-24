[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/glossary/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md)
<!-- zlink-nav:end -->

# Core Glossary

> **What this chapter answers** — short definitions of core terms shared
> across the Core specification. The exact contract for each term is owned
> by its own spec document; this document is the target other documents link
> to on first use.

### Context

zlink's top-level container that holds I/O threads and sockets. The exact
contract is owned by [Context](01-context.en.md).

### I/O thread

A background thread that Context creates and manages. It actually handles
network send and receive.

### socket

An endpoint that sends and receives messages. It must belong to a Context.
The contract is owned by [Socket Common](socket/README.en.md).

### HWM

High-Water Mark. The value that limits the bytes a queue keeps and applies
backpressure.

### backpressure

The behavior that limits a sender's further submissions when downstream
cannot keep up with the processing rate.

### Auto HWM budget

The total byte amount that Core computes from memory inputs and uses as the
basis for dividing HWM among application queues. The contract is owned by
[Auto HWM](systems/06-auto-hwm.en.md).

### directional queue

The physical queue that holds messages for one application direction. It is
counted once even when two endpoints observe the same direction.

### generation

A version number that distinguishes a queue from its predecessor when the
same-direction queue is recreated.

### effective cap

The ceiling placed on the Auto HWM budget. It is the larger of the profile's
fixed cap and the sum of the active queues' minimums.

### retained-credit lease

Credit that transfers only ownership, without releasing bytes, when a
queue's message moves to the Framework. Releasing it returns the read
credit to the original queue.

### water-filling

The distribution method that evenly fills the remaining budget into queues
that have not yet reached their cap, like pouring water.

### completion progress lane

A separate path in DEALER/ROUTER that handles only the progress of terminal
replies and error replies, and is excluded from HWM admission and budget
calculation.
