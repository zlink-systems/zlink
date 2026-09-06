---
title: "Protocol — RAW"
---

[English](https://zlink-systems.github.io/zlink/spec/core/protocol/02-raw/) | 한국어

<!-- zlink-nav:start -->
[프로토콜 목차](README.ko.md) | [이전: ZMP 프로토콜 상세](01-zmp.ko.md) | [다음: 시스템 개요](../systems/README.ko.md)
<!-- zlink-nav:end -->

# Protocol — RAW

> **이 장이 정의하는 것** — ZMP framing 없이 연결하는 RAW 프로토콜의 byte 단위 wire
> format. STREAM socket의 공개 계약은 [Socket — STREAM](../socket/08-stream.ko.md)이
> 소유한다.

## 1. RAW 개요

RAW 프로토콜은 외부 client가 ZMP(zlink Message Protocol) — zlink socket 사이의 wire
framing — 없이 연결할 때 사용된다. STREAM [socket](../glossary.ko.md#socket) 전용
프로토콜로, ZMP를 쓰지 않는 외부 client와 통신하는 용도다. STREAM socket은 이
프로토콜로 지원되는 모든 transport(tcp, ipc, tls, ws, wss)에서 임의의 연결을 수락한다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| STREAM socket의 생성·bind, 수신 모드, 송수신 함수, monitor 이벤트 | [Socket — STREAM](../socket/08-stream.ko.md) |
| ZMP framing으로 연결하는 zlink socket 사이의 wire format | [ZMP 프로토콜 상세](01-zmp.ko.md) |

## 2. Wire format

순수 RAW 모드는 zlink 수준의 framing을 추가하지 않는다. 연결은 투명한 byte stream이다.
peer(연결 상대)가 보낸 byte는 그대로 message 데이터로 전달되고, application이 보낸
byte도 변형 없이 나간다. message 경계는 application이 정의하며, 하부
transport(tcp/ipc/tls/ws/wss)가 byte stream을 제공한다.

이 형태는 다음 설계 의도를 따른다.

- **stream 투명성** — wire에 zlink framing 오버헤드가 없다.
- **zlink 계층 handshake 없음** — transport가 준비되면 바로 데이터가 흐른다.
- **framing의 주체는 application** — 필요한 application-level framing을 application이
  스스로 정한다.

## 3. Packet receive framing (PACKET mode)

framed packet을 수신하려면 application은 첫 successful bind 또는 connect 전에
`ZLINK_STREAM_RECV_MODE_PACKET`을 선택한다. PACKET mode에서 zlink는 투명 stream 대신
length-prefixed(길이 접두사) packet framing을 파싱한다. wire의 byte 배치는 다음과 같다.

```
+------------------+----------------+--------------+------------+
| header_size (2B) | body_size (4B) | header (H B) | body (B B) |
| Big Endian       | Big Endian     |              |            |
+------------------+----------------+--------------+------------+
```

완성한 packet은 `zlink_stream_recv_packet()`이 header와 body를 별도의 `zlink_msg_t`로
caller에게 반환한다. PACKET mode 선택, 수신 함수의 output·ownership, malformed framing
처리의 계약은 [Socket — STREAM의 Packet receive와 framing 절](../socket/08-stream.ko.md#6-packet-receive와-framing)이
소유한다.

## 4. 연결 이벤트

연결 준비와 연결 해제는 in-band application frame이 아니라 socket monitor
이벤트(`ZLINK_EVENT_CONNECTION_READY`, `ZLINK_EVENT_DISCONNECTED`)로 표면화된다.
Transport에서 읽은 0 byte 입력(연결 알림)은 제어 이벤트로 취급되어 application 데이터로
전달되지 않는다. PACKET 모드에서 6 byte prefix가 있는 `header 0 + body 0` packet은 길이 필드를
가진 application 데이터이며, [STREAM packet receive 계약](../socket/08-stream.ko.md)대로 길이 0인
message 두 개로 전달된다.

## 5. 내부 구조

> **이 절의 계약 소유** — RAW wire format의 관찰 가능한 동작은 이 문서의
> [검증 요구](#6-구현-및-contract-test-검증-요구) 절이, STREAM socket의 공개 API 계약은
> [Socket — STREAM](../socket/08-stream.ko.md)이 소유한다. 이 절은 zlink 내부가 RAW
> 연결의 message를 표현하고 encode·decode하는 방법을 설명한다.

### STREAM routing ID와 수신 metadata

STREAM은 연결된 각 client를 `stream_t`가 할당·직렬화한 4 byte routing ID(`uint32`)로
식별한다. 공개 송신 API는 target RID와 `FINAL` payload 하나를 별도 인자로 받으며,
wire에는 payload byte만 전송한다.

수신 시 `stream_t`는 source RID와 payload를 한 번의 routed receive에서 별도 출력으로
반환한다. 공개 `zlink_recv_part()`는 RID를 `source_rid_out_`으로 제공하고 payload를
`FINAL` 단일 part로 반환한다.

### 엔진 구성

- `asio_raw_engine_t` 사용
- `raw_encoder_t`: message byte를 그대로 내보낸다(추가 framing 없음)
- `raw_decoder_t`: 수신한 byte span을 `zlink_msg_t`로 만든다
- routing id는 `stream_t`가 4 byte `uint32` 값으로 할당·직렬화한다

## 6. 구현 및 contract test 검증 요구

공개 표면(STREAM socket 함수, 외부 client의 raw 연결, monitor 이벤트)만으로 다음을
확인한다. 각 항목은 test 하나로 이어진다.

**stream 투명성**
- 외부 client가 보낸 byte는 추가 framing이나 변형 없이 그대로 message 데이터로
  수신된다.
- application이 보낸 byte는 변형 없이 wire로 나간다.
- zlink 계층 handshake 없이 transport가 준비되면 바로 데이터가 흐른다.

**연결 이벤트**
- 연결 준비와 해제는 in-band application frame이 아니라 monitor
  이벤트(`ZLINK_EVENT_CONNECTION_READY`, `ZLINK_EVENT_DISCONNECTED`)로 관찰된다.
- Transport에서 읽은 0 byte 입력은 application 데이터로 전달되지 않는다(제어 이벤트로 취급).
  PACKET 모드의 `0 + 0` packet 전달은 STREAM 계약이 검증한다.

**PACKET mode**
- 첫 successful bind 또는 connect 전에 `ZLINK_STREAM_RECV_MODE_PACKET`을 선택하면 투명
  stream 대신 length-prefixed packet framing(`header_size` 2 byte Big Endian, `body_size`
  4 byte Big Endian, header, body 순)이 파싱되고,
  `zlink_stream_recv_packet()`이 header와 body를 별도의 `zlink_msg_t`로 반환한다.
- PACKET mode의 output·ownership과 malformed framing의 상세 검증은
  [Socket — STREAM의 검증 요구](../socket/08-stream.ko.md#11-구현-및-contract-test-검증-요구)가
  소유한다.
