---
title: "RAW (STREAM) 프로토콜 상세"
---

[English](protocol-raw.en.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: ZMP 프로토콜 상세](protocol-zmp.ko.md)
<!-- zlink-nav:end -->

# RAW (STREAM) 프로토콜 상세

> **이 장의 계약 소유 문서** — STREAM 소켓의 공개 계약은
> [소켓 — STREAM](../spec/core/socket/08-stream.ko.md)이 다룬다. 이 장은 ZMP 프레이밍 없이
> 연결하는 RAW 프로토콜의 바이트 단위 wire format을 설명한다.

RAW 프로토콜은 외부 클라이언트가 ZMP(zlink Message Protocol) 프레이밍 없이 연결할 때 사용된다. STREAM 소켓은 이 프로토콜로 지원되는 모든 transport(tcp, ipc, tls, ws, wss)에서 임의의 연결을 수락한다.

## 1. 개요
STREAM 소켓 전용 프로토콜. ZMP를 쓰지 않는 외부 클라이언트와 통신하는 용도다.

## 2. Wire Format
순수 RAW 모드는 zlink 수준의 프레이밍을 추가하지 않는다. 연결은 투명한 바이트
스트림이다. peer가 보낸 바이트는 그대로 메시지 데이터로 전달되고, 애플리케이션이
보낸 바이트도 변형 없이 나간다. 메시지 경계는 애플리케이션이 정의하며, 하부
transport(tcp/ipc/tls/ws/wss)가 바이트 스트림을 제공한다.

## 3. 설계 의도
- 스트림 투명성: 와이어에 zlink 프레이밍 오버헤드 없음
- zlink 계층 핸드셰이크 없음 — transport가 준비되면 바로 데이터가 흐른다
- 애플리케이션이 필요한 application-level 프레이밍을 스스로 정한다

## 4. STREAM 소켓 내부 API (멀티파트)
zlink 쪽에서는 STREAM 소켓이 연결된 각 클라이언트를 4바이트 routing id로 식별한다
(`uint32`, `stream_t`가 할당·직렬화).

### 4.1 송신 (zlink_send)
```
Frame 1: [Routing ID (4 bytes, uint32)] + MORE flag
Frame 2: [Payload (N bytes)]
```

### 4.2 수신 (zlink_recv)
```
Frame 1: [Routing ID (4 bytes, uint32)] + MORE flag
Frame 2: [Payload (N bytes)]
```

### 4.3 연결 이벤트
연결 준비와 연결 해제는 in-band 애플리케이션 프레임이 아니라 socket monitor
이벤트(`ZLINK_EVENT_CONNECTION_READY`, `ZLINK_EVENT_DISCONNECTED`)로 표면화된다.
raw/packet 경로의 0바이트 payload는 제어 이벤트로 취급되어 애플리케이션
데이터로 전달되지 않는다.

## 5. Packet-Dispatch 프레이밍 (packet handler 모드)
`zlink_stream_packet_handler()`로 packet handler를 등록하면, zlink는 투명
스트림 대신 length-prefixed packet 프레이밍을 파싱한다.
```
+------------------+----------------+--------------+------------+
| header_size (2B) | body_size (4B) | header (H B) | body (B B) |
| Big Endian       | Big Endian     |              |            |
+------------------+----------------+--------------+------------+
```
콜백은 header와 body를 별도의 `zlink_msg_t` part로 받는다.

## 6. 엔진 구현
- `asio_raw_engine_t` 사용
- `raw_encoder_t`: 메시지 바이트를 그대로 내보낸다(추가 프레이밍 없음)
- `raw_decoder_t`: 수신한 바이트 span을 `zlink_msg_t`로 만든다
- routing id는 `stream_t`가 4바이트 `uint32` 값으로 할당·직렬화한다
