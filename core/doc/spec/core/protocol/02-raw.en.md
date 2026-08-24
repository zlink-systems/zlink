---
title: "Protocol — RAW"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/protocol/02-raw/) | English

<!-- zlink-nav:start -->
[Protocol Index](README.en.md) | [Previous: ZMP Protocol Details](01-zmp.en.md) | [Next: Systems Overview](../systems/README.en.md)
<!-- zlink-nav:end -->

# Protocol — RAW

> **What this chapter defines** — The byte-level wire format of the RAW protocol for
> connections without ZMP framing. [Socket — STREAM](../socket/08-stream.en.md)
> owns the public contract of the STREAM socket.

## 1. RAW overview

The RAW protocol is used when an external client connects without ZMP (zlink Message
Protocol), the wire framing between zlink sockets. It is dedicated to the STREAM
[socket](../glossary.en.md#socket) and is used to communicate with external clients
that do not use ZMP. A STREAM socket accepts arbitrary connections over every transport
supported by this protocol (tcp, ipc, tls, ws, wss).

The following documents own the related contracts.

| Related contract | Defining document |
|---|---|
| STREAM socket creation and bind, receive modes, send and receive functions, and monitor events | [Socket — STREAM](../socket/08-stream.en.md) |
| Wire format between zlink sockets connected with ZMP framing | [ZMP Protocol Details](01-zmp.en.md) |

## 2. Wire format

Plain RAW mode adds no zlink-level framing. The connection is a transparent byte
stream. Bytes sent by the peer are delivered unchanged as message data, and bytes sent
by the application go out unchanged. The application defines message boundaries, and
the underlying transport (tcp/ipc/tls/ws/wss) provides the byte stream.

This form follows these design intentions.

- **Stream transparency** — The wire has no zlink framing overhead.
- **No zlink-layer handshake** — Data flows as soon as the transport is ready.
- **The application owns framing** — The application defines any application-level
  framing it needs.

## 3. Packet-dispatch framing (packet handler mode)

When a packet handler is registered with `zlink_stream_packet_handler()`, zlink parses
length-prefixed packet framing instead of a transparent stream. The byte layout on the
wire is as follows.

```
+------------------+----------------+--------------+------------+
| header_size (2B) | body_size (4B) | header (H B) | body (B B) |
| Big Endian       | Big Endian     |              |            |
+------------------+----------------+--------------+------------+
```

The callback receives the header and body as separate `zlink_msg_t` parts. The
[Packet callback section of Socket — STREAM](../socket/08-stream.en.md#7-packet-callback-and-framing)
owns the handler registration and callback contract and the contract for handling
malformed framing.

## 4. Connection events

Connection readiness and disconnection are surfaced through socket monitor events
(`ZLINK_EVENT_CONNECTION_READY`, `ZLINK_EVENT_DISCONNECTED`), not as in-band
application frames. A zero-byte payload on the raw/packet path is treated as a control
event and is not delivered as application data.

## 5. Internals

> **Contract ownership for this section** — The [verification
> requirements](#6-implementation-and-contract-test-verification-requirements) in this
> document own the observable behavior of the RAW wire format, and [Socket —
> STREAM](../socket/08-stream.en.md) owns the public API contract of the STREAM socket.
> This section describes how zlink internals represent, encode, and decode messages on
> RAW connections.

### STREAM socket internal API (multipart)

On the zlink side, a STREAM socket identifies each connected client with a four-byte
routing ID (`uint32`, assigned and serialized by `stream_t`). Both the send and receive
paths use the same internal two-frame layout.

```
Frame 1: [Routing ID (4 bytes, uint32)] + MORE flag
Frame 2: [Payload (N bytes)]
```

### Engine composition

- Uses `asio_raw_engine_t`
- `raw_encoder_t`: emits message bytes unchanged (no additional framing)
- `raw_decoder_t`: turns a received byte span into a `zlink_msg_t`
- `stream_t` assigns and serializes the routing ID as a four-byte `uint32` value

## 6. Implementation and contract-test verification requirements

Verify the following using only the public surface (STREAM socket functions, raw
connections from external clients, and monitor events). Each item maps to one test.

**Stream transparency**
- Bytes sent by an external client are received unchanged as message data, without
  additional framing or transformation.
- Bytes sent by the application go out on the wire unchanged.
- Data flows as soon as the transport is ready, without a zlink-layer handshake.

**Connection events**
- Connection readiness and disconnection are observed through monitor events
  (`ZLINK_EVENT_CONNECTION_READY`, `ZLINK_EVENT_DISCONNECTED`), not as in-band
  application frames.
- A zero-byte payload on the raw/packet path is not delivered as application data (it
  is treated as a control event).

**Packet handler mode**
- When a handler is registered with `zlink_stream_packet_handler()`, length-prefixed
  packet framing (two-byte Big Endian `header_size`, four-byte Big Endian `body_size`,
  followed by header and body) is parsed instead of a transparent stream, and
  the callback receives the header and body as separate `zlink_msg_t` parts.
- The [verification requirements of Socket —
  STREAM](../socket/08-stream.en.md#12-implementation-and-contract-test-verification-requirements)
  own detailed verification of the callback contract and malformed framing.
