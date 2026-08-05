[English](protocol-raw.en.md) | [한국어](protocol-raw.ko.md)

# RAW (STREAM) Protocol Details

The RAW protocol is used when external clients connect without the ZMP framing. STREAM sockets use this protocol to accept arbitrary connections over any supported transport (tcp, ipc, tls, ws, wss).

## 1. Overview
A protocol dedicated to the STREAM socket. Used for communication with external clients that do not use ZMP.

## 2. Wire Format
Plain RAW mode adds no zlink-level framing. The connection is a transparent byte
stream: whatever bytes the peer sends are delivered as message data, and the
bytes the application sends go out unchanged. The application defines its own
message boundaries; the underlying transport (tcp/ipc/tls/ws/wss) provides the byte
stream.

## 3. Design Intent
- Stream transparency: zero zlink framing overhead on the wire
- No zlink-layer handshake (data flows as soon as the transport is ready)
- The application supplies any application-level framing it needs

## 4. STREAM Socket Internal API (Multipart)
On the zlink side, a STREAM socket addresses each connected client by a 4-byte
routing id (`uint32`, assigned and serialized by `stream_t`).

### 4.1 Send (zlink_send)
```
Frame 1: [Routing ID (4 bytes, uint32)] + MORE flag
Frame 2: [Payload (N bytes)]
```

### 4.2 Receive (zlink_recv)
```
Frame 1: [Routing ID (4 bytes, uint32)] + MORE flag
Frame 2: [Payload (N bytes)]
```

### 4.3 Connection Events
Connect readiness and disconnect are surfaced through socket monitor events
(`ZLINK_EVENT_CONNECTION_READY`, `ZLINK_EVENT_DISCONNECTED`), not as in-band
application frames. A zero-size payload on the raw/packet path is treated as a
control event and is not delivered as application data.

## 5. Packet-Dispatch Framing (packet handler mode)
When a packet handler is registered with `zlink_stream_packet_handler()`, zlink
parses a length-prefixed packet framing instead of a transparent stream:
```
+------------------+----------------+--------------+------------+
| header_size (2B) | body_size (4B) | header (H B) | body (B B) |
| Big Endian       | Big Endian     |              |            |
+------------------+----------------+--------------+------------+
```
The callback receives the header and body as separate `zlink_msg_t` parts.

## 6. Engine Implementation
- Uses `asio_raw_engine_t`
- `raw_encoder_t`: emits message bytes directly (no extra framing)
- `raw_decoder_t`: turns the received byte span into a `zlink_msg_t`
- Routing ids are assigned and serialized by `stream_t` as 4-byte `uint32` values
