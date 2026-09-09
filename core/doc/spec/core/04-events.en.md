---
title: "Events"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/04-events/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Errors](03-errors.en.md) | [Next: Polling](05-polling.en.md)
<!-- zlink-nav:end -->

# Events

> **What this chapter defines** — the catalog of socket events and readiness
> values. [Polling](05-polling.en.md) and [Monitoring](06-monitoring.en.md)
> define the consumption paths.

## 1. Events overview

Events observed by applications in zlink Core fall into three families:
monitor events, which subscribe to and record changes in a
[socket](glossary.en.md#socket)'s connection state over a separate channel;
readiness, which indicates that attempting a receive or submit retry now may be
worthwhile; and generic timer fires. This document defines the boundary between
ZLink Core raw event families and readiness meanings—when each event occurs and
what values it carries.

The following documents own the contracts for the paths that actually consume
events.

| Related contract | Defining document |
|---|---|
| readiness consumption—`zlink_poll` and poller wait APIs | [Polling](05-polling.en.md) |
| opening a monitor, recv consumption, event masks and the `zlink_monitor_event_t` declaration, queue overflow, and status counters | [Monitoring](06-monitoring.en.md) |

## 2. Event families

| Family | Source | Delivery API | Meaning |
|---|---|---|---|
| socket monitor | raw socket monitor handle | `zlink_socket_monitor_recv` | bind, connect, handshake, disconnect, protocol, and close |
| poller readiness | raw socket, FD, or generic timer | `zlink_poll`, poller wait | attempting a receive or submit retry is worthwhile |
| timer fire | generic timer handle | `zlink_timer_recv` | an accumulated fire count is available |

A monitor event is an observation record of something that has already
happened. Readiness, by contrast, is a level-triggered state that indicates the
possible presence of work: it is not a record that occurs once and ends, but a
state value that remains observable as true while the condition persists. Do
not assume that one readiness value corresponds one-to-one with one message or
guarantees the success of the next operation.

### Readiness flags

| Flag | Value | Meaning and scope |
|---|---:|---|
| `ZLINK_POLLIN` | `1` | Socket receive or FD read may proceed |
| `ZLINK_POLLOUT` | `2` | Socket submit or FD write may proceed |
| `ZLINK_POLLERR` | `4` | Socket terminal state or FD error |
| `ZLINK_POLLPRI` | `8` | FD urgent data; socket registration returns ENOTSUP |
| `ZLINK_POLLCOMPLETION` | `32` | Completion record ready on a socket with a completion channel |

`ZLINK_POLLCOMPLETION` is accepted only by persistent poller registration for sockets with a
completion channel. FDs and `zlink_poll` reject it with `EINVAL`. `ZLINK_POLLITEMS_DFLT = 16`
is inline capacity, not an event flag; including it in a mask produces `EINVAL`.

## 3. Raw socket lifecycle

A raw socket monitor records endpoint bind/listen, outgoing connect, accept,
handshake success or failure, disconnect, protocol error, and close. Disconnect
reasons distinguish transport error, handshake failure,
[Context](glossary.en.md#context) termination, and unknown. Events contain no
service topology or application payload.

Event identifiers use the `ZLINK_EVENT_` prefix and have the same values as their
`ZLINK_SOCKET_MONITOR_EVENT_` aliases. [Monitoring §3.2](06-monitoring.en.md#32-value-and-flags)
defines `value`, disconnect reasons, protocol errors, lanes, and event flag values.

| Event | Bit | Occurrence |
|---|---|---|
| `CONNECTED` | `1u << 0` | Outgoing transport connected |
| `CONNECT_DELAYED` | `1u << 1` | Connect did not complete immediately |
| `CONNECT_RETRIED` | `1u << 2` | Next reconnect scheduled |
| `LISTENING` | `1u << 3` | Endpoint starts listening |
| `BIND_FAILED` | `1u << 4` | Bind or listener setup failed |
| `ACCEPTED` | `1u << 5` | Inbound transport accepted |
| `ACCEPT_FAILED` | `1u << 6` | Accept failed |
| `CLOSED` | `1u << 7` | Transport or endpoint closed |
| `CLOSE_FAILED` | `1u << 8` | Close or endpoint cleanup failed |
| `DISCONNECTED` | `1u << 9` | Transport disconnected |
| `MONITOR_STOPPED` | `1u << 10` | Monitor stopped notification |
| `HANDSHAKE_FAILED_NO_DETAIL` | `1u << 11` | Handshake failure without protocol detail |
| `CONNECTION_READY` | `1u << 12` | Logical peer ready transition or count snapshot on disconnect |
| `HANDSHAKE_FAILED_PROTOCOL` | `1u << 13` | ZMP handshake protocol validation failed |
| `HANDSHAKE_FAILED_AUTH` | `1u << 14` | TLS certificate verification or client-certificate authentication failed |
| `PEER_WEIGHT_CHANGED` | `1u << 15` | Peer weight applied |

## 4. Receive-flow event

DEALER and ROUTER sockets that support receive flow report their peer's receive-flow state with three
monitor events. The peer communicates its PAUSED or RUNNING state in a
flow-state frame, and Core applies that state to the application pipe from this
socket to that peer—a directional message channel connecting one socket and one
peer.

`ZLINK_EVENT_SEND_FLOW_PAUSED` and `ZLINK_EVENT_SEND_FLOW_RESUMED` occur only
when the peer state on one application pipe of this socket actually transitions
between PAUSED and RUNNING, and only after Core applies that transition to the
pipe. `ZLINK_EVENT_FLOW_STATE_STALE` occurs when Core rejects a frame because
the received flow epoch does not advance on the same connection. No event occurs
for an ordinary data frame, a repeated request for the state that the peer already
maintains, or a flow-state frame that changes nothing. A flow-state frame that
Core internally discards because its physical connection identity does not match
also produces no public monitor event; it increments only the
`flow_state_stale_total` counter defined by [Monitoring](06-monitoring.en.md).

The version number of the applied flow state is called the flow epoch. Each
event carries the following values.

Monitoring defines [receive-flow values and flags](06-monitoring.en.md#32-value-and-flags).

`ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` is absent if another cause, such
as byte [HWM](glossary.en.md#hwm), transport wait, or termination, still blocks
the pipe. A RESUMED event alone therefore does not guarantee that the next send
is accepted.

`FLOW_STATE_STALE_EPOCH` means that the frame's flow epoch did not advance.
`value` is the received epoch. The current epoch can be taken from the preceding PAUSED or
RESUMED event for the same connection only if the same monitor observed that event.

These three events use bits 16, 17, and 18 of the monitor event mask, so
`ZLINK_EVENT_ALL` is `0x7FFFF`. A monitor that specifies a mask directly must
set the corresponding bits to receive these events.

## 5. Ordering and overflow

Within the same monitor queue, Core preserves the order in which it commits
events. It provides no global wall-clock order across different connection
[I/O threads](glossary.en.md#io-thread). [Monitoring](06-monitoring.en.md) owns
the exact contract for queue overflow and status counters.

## 6. Implementation and contract-test verification requirements

Verify the following using only the public surface: the `event`, `value`,
`flags`, and fields of events received through monitor recv, and the
event mask specified when opening a monitor. Each item maps to one unit test.

**Receive-flow event occurrence**

- When a peer state on a DEALER-DEALER or DEALER-ROUTER single connection or a ROUTER-ROUTER two-lane connection actually transitions between PAUSED and RUNNING, `ZLINK_EVENT_SEND_FLOW_PAUSED` or `ZLINK_EVENT_SEND_FLOW_RESUMED` is observed after Core applies that transition to the application pipe.
- No receive-flow event is observed for an ordinary data frame, a repeated request for the state the peer already maintains, or a flow-state frame that changes nothing.
- `ZLINK_EVENT_FLOW_STATE_STALE` is observed when Core rejects a frame because the flow epoch is duplicate or regresses on the same connection.

**Event fields and flags**

- The `value` of a PAUSED or RESUMED event is the flow epoch of the applied state, and the event contains the paused peer's `routing_id`, `connection_id`, and Application `transport_lane`.
- Receive-flow events in both topologies report the `connection_id` and Application `transport_lane`
  of the current Application pipe to which the state was applied. Even when a flow-state frame arrives
  on a ROUTER-ROUTER Completion connection, the event lane does not change to Completion.
- A RESUMED event contains `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` only when clearing the remote pause makes the pipe actually writable. The flag is absent if another cause, such as byte HWM, transport wait, or termination, continues to block the pipe, and a RESUMED event alone does not guarantee acceptance of the next send.
- The `flags` of a STALE event contain `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`, and `value` is the received epoch. The current epoch can be taken from the preceding PAUSED or RESUMED event for the same connection only if the same monitor observed that event.

**Event mask**

- The three receive-flow events use bits 16, 17, and 18 of the monitor event mask, and `ZLINK_EVENT_ALL` is `0x7FFFF`.
- A monitor with an explicitly specified mask receives these events only when the corresponding bits are set.

**Ordering**

- Events received from the same monitor queue preserve the order in which Core commits them.
- No global wall-clock order is guaranteed between events from different connection I/O threads.

[Monitoring](06-monitoring.en.md) owns verification of queue overflow and status
counters.

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Errors](03-errors.en.md) | [Next: Polling](05-polling.en.md)
<!-- zlink-nav:end -->
