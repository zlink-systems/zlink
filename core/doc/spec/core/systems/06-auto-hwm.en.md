[한국어](06-auto-hwm.ko.md) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Per-Connection Memory](05-connection-memory.en.md) | [Next: Core Source Layout](07-core-source-layout.en.md)
<!-- zlink-nav:end -->

# Auto HWM Internals

This document defines the state that Core maintainers may read and update on
the message-processing path when implementing Auto HWM memory limits. The
[Context specification](../01-context.en.md#auto-hwm-memory-budget-calculation)
and the [Socket specification](../socket/README.en.md#transportbuffer)
own the application-visible budget calculation, HWM scope, and errors.

## Value limited by HWM

Auto HWM divides a context memory budget among application directional queues.
A message is admitted by comparing the target physical queue's unreturned
charge with its applied HWM, not by comparing context-wide usage.

```text
frameCharge = payloadBytes + sizeof(msg_t)

outstandingCharge =
    provisionalCharge
  + committedQueueCharge
  + retainedLeaseCharge
```

The fixed `sizeof(msg_t)` component is not an allocator measurement. It keeps
empty frames from consuming zero HWM while they still occupy a queue slot and
a message object. Payload-only accounting cannot bound a stream of empty
single-part messages or empty multipart frames.

`provisionalCharge` is reserved before a decoder allocates a payload buffer.
`committedQueueCharge` belongs to frames stored by the queue. A frame retained
by the application moves to `retainedLeaseCharge`. Moving ownership does not
change the amount for which the writer has not received credit.

For example, when the HWM is 1,024 bytes and the unreturned charge is 900
bytes, ordinary admission accepts only a frame whose charge is at most 124
bytes. The state of another queue and the context-wide total do not change this
decision.

## Separation of responsibilities

| Processing location | Input | Result |
|---|---|---|
| Budget planner | Memory inputs, profile, application queue set | Target HWM per queue |
| Queue configuration | Target HWM, applied value, queue generation | HWM applied to that generation |
| Message processing | Target queue charge and frame charge | Admission or backpressure |
| Snapshot | Per-queue HWM and charge | Context query result |

The planner runs for option and queue-lifecycle changes. Context aggregates
and snapshot metrics are not message-admission inputs.

## Message-processing sequence

1. The writer or decoder adds the fixed frame cost to the payload size.
2. An overflowing sum is rejected.
3. Before allocating a payload buffer, a decoder reserves the candidate charge
   in target queue-local state. An application writer skips this step for an
   already-created message.
4. A nonzero applied HWM rejects a candidate that would make the unreturned
   charge exceed the HWM, except for the public empty-queue oversize rule.
5. Enqueue converts a provisional charge to committed state without adding it
   again.
6. Dequeue removes committed charge. A retained receive transfers the same
   charge to its lease; an ordinary receive returns byte credit to the writer.
7. Drop, allocation failure, protocol failure, and shutdown return only the
   charge that the operation owns, exactly once.

Normal frame processing reads and updates only state created with its queue.

## Multipart and oversize messages

Multipart messages accumulate each frame's charge. After reading a wire frame
length, a decoder reserves that frame before allocating its buffer. The final
frame publishes the multipart without charging preceding frames again.

An incremental multipart that reaches HWM stops before allocating its next
frame. Discarding a multipart returns every charge it reserved or committed.

An empty queue may admit one complete message whose total charge is known at
admission even when it exceeds HWM. The exception does not admit two messages
at once and does not bypass `ZLINK_OPT_MAXMSGSIZE`. The
[Socket specification](../socket/README.en.md#transportbuffer) owns
the public rule.

## Retained receive and queue generations

A retained receive lets the application keep a dequeued frame's memory. It
does not return charge at dequeue. Lease release returns credit to the writer
of the originating queue generation.

Detach and reconnect create a new generation. Releasing an old lease neither
decreases the new generation's charge nor wakes its writer. A retired
generation keeps only the return target until its final lease and reservation
finish.

## HWM changes

An increase applies to the current queue generation. A decrease below the
unreturned charge preserves admitted frames and blocks new admission. Core
applies the lower HWM after the charge reaches the target.

DEALER and ROUTER completion queues do not use application HWM so terminal and
error replies can progress. Monitor queues are excluded from application
budget distribution.

## Message-path cost limits

Send, receive, and decoder admission do not perform:

- context-wide mutex acquisition;
- global map lookup by queue or reservation ID;
- per-frame heap allocation for reservations;
- per-frame updates of context-wide current, provisional, or peak totals;
- global atomic updates for metrics not used by HWM admission;
- usage lookup for another physical queue.

A decoder or pipe stores its reservation inline: target queue reference,
generation, reserved charge, and active state. A lifecycle registry may manage
attach, detach, and retired generations, but normal frame admission does not
look it up.

## Snapshots and metrics

A snapshot composes context totals from queue-local state when requested. It
may take registry locks needed for the query, but it does not serialize every
queue through a lock shared with message processing.

Peak metrics retain the largest aggregate observed when a snapshot or an
automatic HWM recalculation collects the queue-local values. Because the hot
path does not update a context-wide total for every frame, a value that exists
only between two observation boundaries may not appear in the peak. Metrics
reset and repeated snapshot reads do not alter admission or writer credit.

## Implementation locations

| Responsibility | Implementation location |
|---|---|
| Profile bounds and per-queue HWM calculation | `auto_hwm_policy.*` |
| Context inputs, recalculation, snapshot API | `ctx_auto_hwm_*` |
| Physical queue identity and generation | `ctx_physical_queue_registry.*` |
| Queue-local charge, admission, and byte credit | `pipe.*` |
| Pre-allocation reservation | `zmp_decoder.*`, `session_base_pipe_io.cpp`, `pipe.*` |
| Retained lease release | Retained receive API and queue lifecycle code |

## Verification requirements

Functional tests verify that:

- empty single-part and multipart frames consume finite HWM;
- incremental multipart stops before allocation and rollback leaves no reservation;
- an empty queue admits only one complete oversize message;
- ordinary dequeue returns credit and retained dequeue waits for lease release;
- detach with a retained lease never returns credit to a new generation;
- HWM shrink preserves existing frames and applies after drain;
- completion and monitor queues do not affect application HWM calculation;
- snapshots and metrics reset do not alter admission for the same message sequence.
