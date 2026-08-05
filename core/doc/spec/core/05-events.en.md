[한국어](05-events.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md) · [Monitoring](07-monitoring.en.md) · [Polling](06-polling.en.md)

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

## 3. Ordering and overflow

One monitor queue preserves the order in which Core commits events. No global
wall-clock order is provided across connection I/O threads. [Monitoring](07-monitoring.en.md)
owns the exact queue-overflow and status-counter contract.
