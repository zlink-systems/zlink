[한국어](04-events.ko.md) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Errors](03-errors.en.md) | [Next: Polling](05-polling.en.md)
<!-- zlink-nav:end -->

# Events and readiness catalog

This document defines the boundaries among ZLink Core raw event families
and readiness meanings.

## 1. Event families

| Family | Source | Delivery API | Meaning |
|---|---|---|---|
| socket monitor | raw-socket monitor handle | handler or receive | bind, connect, handshake, disconnect, protocol, and close |
| poller readiness | raw socket, FD, or generic timer | `zlink_poll` or poller wait | receiving or retrying a submit is worthwhile |
| timer fire | generic timer handle | handler or timer receive | an accumulated fire count is available |

A monitor event is an observation record. Readiness is a level-triggered
indication that work may be available. One readiness signal does not correspond
to one message and does not guarantee that the next operation succeeds.

## 2. Raw-socket lifecycle

A raw-socket monitor records endpoint bind/listen, outgoing connect, accept,
handshake success or failure, disconnect, protocol error, and close. Disconnect
reasons distinguish transport error, handshake failure, Context termination,
and unknown. Events contain no service topology or application payload.

## 3. Receive-flow events

A paired DEALER/ROUTER socket reports the receive-flow state of its peers with
three monitor events. `ZLINK_EVENT_SEND_FLOW_PAUSED` and
`ZLINK_EVENT_SEND_FLOW_RESUMED` are emitted only when a peer's state actually
flips between PAUSED and RUNNING on one application pipe of this socket, after
that flip has been applied to the pipe. `ZLINK_EVENT_FLOW_STATE_STALE` is
emitted when Core rejects a flow-state frame as stale or duplicate. Core emits
no event for an ordinary data frame, for a repeated request of the state a peer
already holds, and for a flow-state frame that changes nothing.

| Event | `value` | `flags` | Other fields |
|---|---|---|---|
| `ZLINK_EVENT_SEND_FLOW_PAUSED` | flow epoch of the applied state | none | `routing_id` of the paused peer, `transport_pair_id`, `transport_pair_generation` |
| `ZLINK_EVENT_SEND_FLOW_RESUMED` | flow epoch of the applied state | `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` when clearing the remote pause left the pipe writable | same as PAUSED |
| `ZLINK_EVENT_FLOW_STATE_STALE` | received generation or received epoch, selected by the reason flag | exactly one of `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION` or `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH` | `transport_pair_id`, `transport_pair_generation` holding the current generation |

`ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` is absent when another cause —
byte HWM, a transport wait, or termination — still blocks the pipe, so a
RESUMED event alone does not promise that the next send is admitted.

The two stale reasons select what `value` carries, so it is never ambiguous.
With `FLOW_STATE_STALE_GENERATION`, the frame named a different connection
generation; `value` is the received generation and `transport_pair_generation`
is the current one. With `FLOW_STATE_STALE_EPOCH`, the frame belongs to the
current generation but its epoch did not advance; `value` is the received
epoch, and the current epoch is the one reported by the preceding PAUSED or
RESUMED event for the same pair.

The three events occupy bits 16, 17, and 18 of the monitor event mask, so
`ZLINK_EVENT_ALL` is `0x7FFFF`. A monitor that selects an explicit mask
receives them only when it sets those bits.

## 4. Ordering and overflow

One monitor queue preserves the order in which Core commits events. No global
wall-clock order is provided across connection I/O threads. [Monitoring](06-monitoring.en.md)
owns the exact queue-overflow and status-counter contract.
