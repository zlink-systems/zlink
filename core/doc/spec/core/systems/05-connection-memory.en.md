---
title: "Connection Memory"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/05-connection-memory/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Thread Safety](04-thread-safety.en.md) | [Next: Auto HWM](06-auto-hwm.en.md)
<!-- zlink-nav:end -->

# Connection Memory

> **What this chapter defines** — how much memory a single connection actually allocates
> (fixed costs and variable costs proportional to the HWM).

## 1. Connection memory overview

Each transport connection uses two kinds of memory. One is the **fixed cost** allocated when the
connection is created. Its components differ between inproc and socket-based transports. The other
is the **variable cost** of message storage retained by queues and pending-request lifecycle state.
It is not a fixed connection cost. Queue storage grows with each frame's actual accounted bytes and the
[HWM](../glossary.en.md#hwm), which limits the bytes retained in a queue.

This document defines the components of these two costs and the limitations of observing and
measuring them. Its intended audience is developers who estimate per-connection memory usage and
perform capacity planning.

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| Memory budget calculation, byte accounting, and HWM admission | [Auto HWM](06-auto-hwm.en.md) |
| [socket](../glossary.en.md#socket) options and HWM observation behavior | [Socket Common](../socket/README.en.md) |
| Short definitions of terms | [Core Glossary](../glossary.en.md) |

## 2. Fixed components

The components allocated when a connection is created, regardless of traffic volume, differ by
transport as follows.

| Transport | Fixed components |
|---|---|
| inproc | A pipepair that directly connects two socket endpoints. It creates no session or engine and has no protocol handshake state or operating-system socket. |
| Socket-based transport | Session, engine, pipe endpoints and queue chunks, routing ID and endpoint metadata, protocol handshake state, and operating-system socket structures. TLS adds record and handshake storage. |

## 3. Variable components

> **Contract owner for this section** — [Auto HWM](06-auto-hwm.en.md) owns the public contracts and
> internal implementation for byte accounting, HWM admission, and budget calculation. This section
> summarizes their results from the perspective of the memory cost of one connection.

### 3.1 Frame byte charge

A physical queue that stores messages for one application direction is called a
[directional queue](../glossary.en.md#directional-queue). For each frame, a directional pipe
calculates the payload plus `sizeof(zlink_msg_t)` as the byte charge. This charge is reserved and
returned in the following order.

1. Immediately after checking the frame length, the decoder that interprets received wire bytes as
   a frame first acquires provisional credit from the origin queue. Only then does it allocate the
   payload buffer.
2. For the final frame of a multipart message, it converts the same provisional total into a
   committed message without incrementing the counter again.
3. Write failure, rollback, close, and detach return exactly once the charge of each provisional or
   committed frame actually removed.

### 3.2 Empty-pipe oversize exception

An empty application pipe admits one complete message larger than the HWM, provided that the
message does not exceed the socket's maximum message size, and then stops subsequent writes. This
exception does not apply to an unfinished multipart message.

### 3.3 Pending-request lifecycle state

Pending-map entries, callbacks, and timeout state grow with the number of live requests. The
[pending-request admission limit](06-auto-hwm.en.md#pending-request-admission) uses a per-physical-pair
32 MiB size-weighted work budget and unresolved-request count limit of 16,384 to bound completion
liveness. Work charge is neither actual allocator bytes nor retained payload bytes and is not
included in queue-HWM current or snapshot values. Application HWM limits only frames resident in
the queue and is not reused as a separate lifecycle limit for unresolved correlation.

### 3.4 Completion progress lane

The DEALER/ROUTER [completion progress lane](../glossary.en.md#completion-progress-lane) is a
separate path that advances terminal replies and error replies and synchronizes receive-flow-state
frames between peers. Byte HWM, LWM, manual HWM, and Core budget reservation do not apply to this
lane. Even when an application pipe is full, valid completion records and receive-flow-state frames
are admitted if the connection remains available and allocation succeeds.

The completion lane preserves the `SNDBUF`/`RCVBUF` default of `-1`, leaving OS defaults and
autotuning unchanged for every transport. When the application supplies a nonnegative value, Core
caps it at 64 KiB for the completion lane and applies it consistently to the underlying TCP socket
for TCP, TLS, WS, and WSS.

## 4. Measurement and limitations

The monitor that reports runtime memory state separately exposes current bytes for application
queues and completion lanes, as well as oversize-admission history. The
`application_accounted_bytes`, `outstanding_application_lease_count`,
`deferred_origin_credit_bytes`, and `retired_queue_count` fields that were allocated to the removed
retained-credit feature remain for ABI compatibility and are always zero. The
[Core budget](../glossary.en.md#auto-hwm-budget) — the total number of bytes that Core uses as the
basis for distributing HWM among application queues after calculating it from memory inputs — is
the normal-state basis for distributing per-pipe HWM, not a hard cap on actual context memory
usage. This value explains Core accounting but is not an exact measurement of process resident
memory.

Kernel buffers may grow according to platform autotuning. TLS adds record and handshake storage.
Monitor snapshots report the applied HWM plan but do not measure all allocator and kernel overhead.

## 5. Capacity planning

Capacity planning measures each of the following three values using the production transport and
message-size distribution.

- Idle memory
- Residual memory after traffic
- Burst peak memory

## 6. Implementation and contract-test verification requirements

[Auto HWM verification requirements](06-auto-hwm.en.md#5-implementation-and-contract-test-verification-requirements)
own the detailed verification items for byte accounting, admission, dequeue credit, and the
oversize exception. From the connection-memory perspective, verify the following items. Each item
maps to one test.

**Byte charge**

- Accepting one frame increases the pipe's accounted bytes by the payload plus `sizeof(zlink_msg_t)`.
- The final frame of a multipart message only converts the provisional total into a committed message; it does not increment the counter again.
- After write failure, rollback, close, or detach, the provisional and committed charges of the frames actually removed are returned exactly once.

**HWM and dequeue credit**

- The application directional HWM applies only to physical-frame bytes currently held by the Core queue.
- When the Core queue dequeues a complete message to the binding, that message's queue charge ends and writer credit is returned. A binding or application continuing to hold the payload does not add it back to Core HWM accounting.

**Oversize and completion**

- Sending an empty application pipe a complete message that is within the socket's maximum message size but larger than the HWM admits one message and then stops subsequent writes.
- The empty-pipe oversize exception does not apply to an unfinished multipart message.
- Even when an application pipe is full, a valid completion record is admitted if the connection remains available and allocation succeeds.
- RUNNING and PAUSED receive-flow-state frames are synchronized through the DEALER/ROUTER completion progress lane.
- Byte HWM, LWM, manual HWM, and Core budget reservation do not apply to the completion progress lane.

**Measurement**

- The monitor separately reports current bytes for application queues and completions, as well as oversize-admission history, and reports the retained-credit compatibility fields as zero.
- The Core budget is the normal-state basis for distributing per-pipe HWM and does not act as a hard cap on actual context memory usage. [Auto HWM](06-auto-hwm.en.md) owns the detailed admission contract.
