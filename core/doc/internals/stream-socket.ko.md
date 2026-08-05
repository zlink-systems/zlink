---
title: "STREAM 소켓 WS/WSS 최적화"
---

[English](stream-socket.en.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Source layout](core-source-layout.ko.md) | [다음: Socket option 기본값](socket-option-defaults.ko.md)
<!-- zlink-nav:end -->

# STREAM 소켓 WS/WSS 최적화

> **이 장의 계약 소유 문서** — STREAM 소켓의 공개 계약은
> [소켓 — STREAM](../spec/core/socket/08-stream.ko.md)이 다룬다. 이 장은 그 중 WS/WSS
> 경로의 내부 최적화 구조를 설명한다.

## 1. 개요

STREAM 소켓은 ZMP(zlink Message Protocol) 핸드셰이크 없이 연결하는 외부 클라이언트
(웹 브라우저, 게임 클라이언트 등)와 RAW 통신을 지원한다. tcp, tls, ws, wss transport를
지원하며, 특히 WS/WSS 경로의 성능 최적화에 집중한다.

## 2. 아키텍처

### 2.1 컴포넌트 구성

| 컴포넌트 | 파일 | 역할 |
|----------|------|------|
| stream_t | src/runtime/sockets/stream/stream.cpp | STREAM 소켓 로직 |
| raw_encoder_t | src/runtime/protocol/raw_encoder.cpp | passthrough 인코딩 (framing 없음) |
| raw_decoder_t | src/runtime/protocol/raw_decoder.cpp | passthrough 디코딩 (바이트 span -> msg_t) |
| asio_raw_engine_t | src/runtime/engine/asio/asio_raw_engine.cpp | RAW I/O 엔진 |
| ws_transport_t | src/runtime/transports/ws/ | WebSocket transport |
| wss_transport_t | src/runtime/transports/tls/ | WebSocket + TLS transport |

### 2.2 데이터 흐름

```mermaid
sequenceDiagram
    participant App as Application
    participant SS as Stream Socket
    participant Eng as Engine
    participant Tr as Transport

    App->>SS: zlink_send(rid + data)
    SS->>Eng: pipe_t::write()
    Eng->>Tr: raw_encode (passthrough 바이트, framing 없음)
    Tr->>Tr: ws::write
```

## 3. WS/WSS 성능 특성

### 3.1 Read Path
- 데이터는 Beast read buffer(`message_buffer`)에서 출력 `msg_t` 로
  복사된다 (delivery 시 단일 copy).

### 3.2 Write Path
- `msg_t` payload 를 Beast write 버퍼로 직접 전달한다 (중간 copy 없음).

### 3.3 Beast Write Buffer
- Beast write 버퍼 기본값은 64KB다. WS write는 전달받은 버퍼 하나를
  단일 binary frame(`async_write` 한 번)으로 보낸다.

### 3.4 프레임 분할
- `auto_fragment(false)` — 논리 메시지 하나가 하나의 WebSocket 프레임에
  대응한다.

## 4. 측정 처리량

표준 벤치마크 머신의 단일 socket 대표 처리량:

| Transport | Throughput |
|-----------|------------|
| TCP       | 1493 MB/s  |
| WS        |  696 MB/s  |
| WSS 1KB   |  382 MB/s  |

WS 프레이밍을 택해서 얻는 이득은 대용량 메시지에서 가장 크다. 64KB 이상
payload 에서는 WS 가 TCP 라인 레이트에 근접하고, WSS 비용은 TLS 암호화
오버헤드가 좌우한다.

## 5. 설계 트레이드오프

- Speculative write 미지원 (WebSocket 프레임 기반)
- Gather write는 WS/WSS에서 미지원 (`supports_gather_write()`가 false)
- TLS/WSS는 암호화 오버헤드 존재

## 6. Packet Handler 수신 모드

STREAM 소켓에는 서로 배타적인 수신 모드가 셋 있다. 소켓 하나당 하나만
활성화할 수 있으며, 같은 소켓에 두 번째 활성화를 시도하면 `EBUSY`로
실패한다.

| 모드 | 활성화 방식 | 전달 형태 |
|------|-------------|-----------|
| Raw recv | 기본 | `zlink_recv()`가 read 단위로 raw bytes 반환 |
| Raw callback | `zlink_recv_handler()` | `zlink_socket_msg_handler_fn` 이 raw bytes 를 받는다 |
| Packet callback | `zlink_stream_packet_handler()` | `zlink_stream_packet_handler_fn` 이 header / body 로 이미 분리된 `zlink_msg_t` 를 받는다 |

Packet handler 모드는 raw STREAM 바이트 파이프 위에 `header + body` 프레이밍을
올리는 애플리케이션 프로토콜을 위한 것이다 — 예를 들어 주문 처리 게이트웨이가
작은 제어 헤더 뒤에 큰 payload를 싣는 경우다. 호출자마다 똑같은
length-prefix(길이 접두사) 디코더와 버퍼링 상태 머신을 거듭 구현하는
대신, STREAM 이 내부에서 frame 을 파싱하고 이미 할당된
`zlink_msg_t` 를 콜백에 넘긴다.

### 6.1 Wire framing

각 논리 packet 은 wire 에 다음 형식으로 실린다:

```
+------------------+--------------------+----------------+-------------------+
| u16 header_size  | u32 body_size      | header bytes   | body bytes        |
| (big-endian)     | (big-endian)       | (header_size)  | (body_size)       |
+------------------+--------------------+----------------+-------------------+
```

- `header_size` 는 2-byte big-endian unsigned integer.
- `body_size` 는 4-byte big-endian unsigned integer.
- 두 size 는 모두 `0` 일 수 있다. `header_size=0 && body_size=0` 인 패킷도
  콜백을 그대로 유발하며, header 와 body 가 비어 있어도 non-`NULL`
  인 `zlink_msg_t` 두 개로 전달된다.
- size 검사는 `maxmsgsize` 가 양수로 설정된 경우에만 적용된다(기본값 `-1`은
  무제한). 설정된 한계를 넘는 size 광고는 malformed framing 으로 취급된다 (6.4 참고).

### 6.2 Per-connection 누적기

들어오는 바이트는 각 연결(pipe)의 packet state(`pipe_t::_stream_packet_state`)를
거친다. handler 는 `pipe_->stream_packet_state()` 로 이 상태에 접근한다.

```
  wire bytes (arbitrary fragmentation)
         |
         v
  +-------------------------+
  | pipe packet state       |
  |   stage: prefix_stage   |
  |          header_stage   |
  |          body_stage     |
  +-------------------------+
         |
         v
  callback(stream, source_rid, header_msg, body_msg, userdata)
```

먼저 length field 가 파싱된다. `header_size` 와 `body_size` 가 모두 확정되면
이후 도착하는 바이트가 해당 연결의 packet state 의 header / body buffer 에
누적된다. 패킷이 완성되면 그 누적 buffer 들이 새로 초기화된 `zlink_msg_t`
header / body 로 move(zero-copy)되어 콜백에 전달된다. Delivery 시점에는
추가 copy 가 없다 — move 가 조립된 buffer 를 콜백이 받을 message 로 옮긴다.

### 6.3 Callback 규약

Signature:

```
zlink_stream_packet_handler_fn(stream,
                               source_rid,    // borrowed view
                               header_msg,    // ownership 이전
                               body_msg,      // ownership 이전
                               userdata)
```

- `source_rid` 는 콜백 실행 동안만 유효한 빌린 참조(borrowed view)다. 콜백
  뒤에도 보존하려면 복사해야 한다.
- `header_msg` 와 `body_msg` 는 wire size 가 `0` 이어도 항상
  non-`NULL` 로 전달된다. 두 메시지의 ownership 이 콜백으로 넘어가며,
  콜백이 `zlink_msg_close()` 로 닫을 책임을 진다.
- 같은 `source_rid` 에서 오는 패킷들은 직렬화된다. 같은 피어의 뒤
  패킷이 앞 패킷을 앞지를 수 없다. 서로 다른 `source_rid` 의 패킷
  은 서로 다른 worker 스레드에서 병렬로 dispatch될 수 있다.
- 콜백 안에서의 self-close 는 raw `zlink_recv_handler` 케이스와 같은
  규칙을 따른다. 콜백 안에서 수신 모드를 바꾸거나 소켓을 닫으려
  하면 `EBUSY` 로 실패한다.

### 6.4 Malformed framing

다음 상황은 malformed 로 보고 해당 연결을 닫는다.

- 선언된 `header_size` 또는 `body_size` 가 내부 한계를 넘는 경우.
- Length field 는 도착했지만 전체 패킷이 도착하기 전에 피어가 close /
  reset 되는 경우 — 즉 mid-length 또는 mid-payload close.

이 경우 STREAM monitor 에 해당 `source_rid` 의 disconnect 이벤트로
노출된다. 불완전한 패킷은 절대 콜백으로 전달되지 않으며,
연결과 함께 decoder state 도 폐기된다.

### 6.5 왜 STREAM 안에서 decode 하는가

애플리케이션마다 따로 하는 대신 STREAM 안에서 decode 하도록 둔 이유는 두 가지다.

- **복사 한 번 감소.** 애플리케이션이 "조립된" contiguous buffer 를 한 번
  만졌다가 다시 쪼갤 필요가 없다. 누적 buffer 가 header / body message 로
  move(zero-copy)된다.
- **순서 보장.** Per-`source_rid` 직렬화를 decoder 쪽에서 강제하므로,
  호출자가 raw byte delivery 위에 별도 reorder 로직을 올릴 필요가 없다.

## 7. 현재 STREAM 런타임 기본값

STREAM 은 transport 전반에 공통된 기본 성능 프로파일을 쓴다.
STREAM 외 공통 소켓 기본값은
[socket-option-defaults.ko.md](socket-option-defaults.ko.md)를 참고한다.

### 7.1 내부 상수 고정 항목

아래 값들은 내부 상수로 고정되며 STREAM env 로 제어하지 않는다:
- handler allocator: 활성
- read drain: 활성
- speculative write: STREAM/TCP 경로에서 상시 on 고정
- RX slab buffering: 활성
- speculative write byte budget: `2097152`
- read drain max loops: `64`
- read drain max bytes: `1048576`

### 7.2 소켓/리스너 기본값

- backlog: `65536`
- `sndhwm` / `rcvhwm`: routed 역할 auto HWM floor에서 시작
- `sndbuf` / `rcvbuf`: 기본값 `-1`. OS 기본 버퍼와 TCP 자동 조정에 맡김
- accept 동시성(STREAM 전용): 기본 `4`, 최대 `128`
- 세션 스케줄러(STREAM): 기본 `rr`

### 7.3 현재 유지되는 STREAM 런타임 환경변수

- `ZLINK_ASIO_STREAM_ACCEPT_CONCURRENCY`: 기본 `4`, 최대 `128`로 제한
- `ZLINK_ASIO_STREAM_SESSION_SCHED` (`rr|minload`): 기본 `rr`
- `ZLINK_ASIO_STREAM_ENABLE_NON_TCP_SPEC_READ`: 기본 비활성
- `ZLINK_ASIO_STREAM_DISABLE_GATHER`: 기본 비활성이라 STREAM gather-write 는 유지됨
- `ZLINK_ASIO_STREAM_GATHER_THRESHOLD`: 기본 `1024`
- `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD`: 기본 `0`
- `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP`: 기본 `4096`
- `ZLINK_ASIO_STREAM_BATCH_SIZE`: 기본 `4096`
- `ZLINK_ASIO_STREAM_BATCH_HEADROOM`: 기본 `64`

## 8. Peer rid disconnect

STREAM의 public routing id는 서버가 연결별로 부여한 4바이트 connection id다.
`zlink_disconnect_rid()`는 이 id를 `uint32_t`로 해석해 STREAM 라우팅 맵에서
pipe를 찾고 종료 요청을 넣는다. 4바이트가 아닌 rid는 잘못된 인자로 실패한다.
