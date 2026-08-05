---
title: "STREAM 소켓"
---

[English](03-5-stream.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: ROUTER](03-4-router.ko.md) | [다음: 프록시 패턴](03-6-proxy.ko.md)
<!-- zlink-nav:end -->

# STREAM 소켓

> **이 장의 계약 소유 문서** — [STREAM socket 스펙](../spec/core/socket/08-stream.ko.md)이
> 다룬다. 이 챕터는 그 계약을 언어별 예제로 보여준다.

## 1. 개요

STREAM 소켓은 **외부 RAW 클라이언트**와 통신하기 위한 **서버 전용** 소켓이다.

핵심 규칙:
- `ZLINK_SOCKET_STREAM`은 `zlink_bind()`만 지원한다.
- `ZLINK_SOCKET_STREAM`에 `zlink_connect()`를 호출하면 `EOPNOTSUPP`를 반환한다.
- 클라이언트는 zlink STREAM 소켓이 아니라 OS/Asio/WebSocket 등의 **raw client**를 사용해야 한다.
- STREAM은 raw 바이트 스트림을 그대로 전달한다. **framing(패킷 경계)은 사용자가 정의**해야 한다.
- zlink API에서 수신/송신 시 `source_rid`(서버가 자동 할당한 4B 연결 식별자)로 클라이언트를 구분한다([Routing ID](08-routing-id.ko.md) 참고).

유효 조합:

```
external raw client  <---- RAW 바이트 스트림 (framing 없음) ---->  STREAM(server)
```

> STREAM은 zlink 내부 소켓(PAIR/PUB/SUB/DEALER/ROUTER)과 직접 호환되지 않는다.

---

## 2. 서버 생성/바인드

```c
void *stream = zlink_socket(ctx, ZLINK_SOCKET_STREAM);
int linger = 0;
zlink_set_option(stream, ZLINK_OPT_LINGER, &linger, sizeof(linger));
zlink_bind(stream, "tcp://0.0.0.0:8080");
```

지원 transport(서버 bind):
- `tcp://`
- `tls://`
- `ws://`
- `wss://`

---

## 3. STREAM 고유 동작

STREAM은 기반 소켓 계열(raw socket family)에서 유일한 예외 타입이다. 한 핸들에서 세
가지 수신 모델 중 정확히 하나를 고른다.

- **직접 수신(raw recv)**: `zlink_recv()`로 transport 조각을 직접 가져온다. poller의
  `ZLINK_POLLIN`과 함께 사용한다.
- **콜백 수신(raw callback)**: `zlink_recv_handler()`로 수신 조각을 콜백으로 받는다.
  이벤트 기반(event-driven) 서버에 적합하다.
- **패킷 콜백(packet callback)**: `zlink_stream_packet_handler()`로 고정 framing(framing,
  패킷 경계를 구분하는 방식) 규약(2B header size + 4B body size + header + body, big-endian)을
  따르는 패킷을 조립된 header/body 형태로 받는다.

세 모델은 상호 배타이며, 한 핸들에서 두 번째 모드로 전환하려 하면
`EBUSY`로 실패한다. 응용은 필요에 맞는 모드 하나만 고른다.

STREAM만의 고유 동작은 다음과 같다.

- `source_rid`는 서버가 연결별로 자동 할당하며,
  고정 4바이트(`uint32`, big-endian)이다.
- 특정 클라이언트를 끊어야 하면 콜백이나 recv에서 받은 `source_rid`를
  `zlink_disconnect_rid()`에 넘긴다. STREAM의 대상 rid는 반드시 4바이트다.
- connect/disconnect는 in-band 데이터 마커가 **아니다**. 소켓 monitor의
  `ZLINK_EVENT_CONNECTION_READY` / `ZLINK_EVENT_DISCONNECTED` 이벤트로
  보고되며 각각 4바이트 `routing_id`를 담는다. 우연히 1바이트 `0x00`/`0x01`인
  raw payload는 일반 데이터로 전달된다.

---

## 4. 콜백 예시

STREAM raw 콜백에서 전달되는 모든 part는 애플리케이션 데이터다.
connect/disconnect는 소켓 monitor로 관찰한다([Monitoring](06-monitoring.ko.md) 참고).

```c
void on_message(const zlink_routing_id_t *source_rid,
                zlink_msg_t *parts, size_t part_count,
                void *userdata)
{
    for (size_t i = 0; i < part_count; i++) {
        void *data = zlink_msg_data(&parts[i]);
        size_t size = zlink_msg_size(&parts[i]);

        /* echo reply */
        zlink_msg_t reply;
        zlink_msg_init_size(&reply, size);
        memcpy(zlink_msg_data(&reply), data, size);
        zlink_send_rid(stream, source_rid, &reply, 1, 0);

        zlink_msg_close(&parts[i]);
    }
}

/* Attach callback dispatch (콜백 밖에서 해제 가능; 콜백 실행 중 detach/close는 EBUSY) */
zlink_recv_handler(stream, on_message, NULL);
```

### 주요 사항

| 항목 | 설명 |
|---|---|
| Attach API | `zlink_recv_handler()` |
| 콜백 | `zlink_socket_msg_handler_fn` |
| 수명 | 콜백 밖에서 dispatch 해제 가능. 콜백 실행 중 detach/close는 `EBUSY` |
| framing | transport에서 수신된 raw 바이트 |
| 전송 | `zlink_send_rid()` |

> 송신 큐가 가득 차면(HWM, 고수위 표시) `zlink_send_rid()`는 블록(기본) 또는
> `ZLINK_DONTWAIT` 로 `ZLINK_SUBMIT_BACKPRESSURED` 를 반환한다.
> 배압(backpressure) 패턴은 [성능 가이드](10-performance.ko.md)를 참고.

- 수신 콜백은 한 번에 하나만 등록할 수 있으며, 이미 등록된 상태에서 attach를
  호출하면 `errno=EBUSY`와 함께 `ZLINK_HANDLER_BUSY`를 반환한다.
- 수신 콜백이 활성인 동안 direct recv 계열과 data-plane `POLLIN`은
  `EBUSY`다.
- 콜백 내부에서 close를 호출하는 것은 지원되지 않는다 (`EBUSY` 실패).

---

## 4.1 패킷 콜백 모드

고정 framing 규약(2바이트 big-endian header size + 4바이트 big-endian
body size + header payload + body payload)을 사용하는 상위 프로토콜에서는
`zlink_stream_packet_handler()`로 패킷 단위 콜백을 등록할 수 있다.
core가 조각(fragment) 누적과 길이 해석을 직접 처리하므로 응용은 header/body를
그대로 받아 처리한다.

```c
void on_packet(void *stream,
               const zlink_routing_id_t *source_rid,
               zlink_msg_t *header,
               zlink_msg_t *body,
               void *userdata)
{
    /* header_size == 0 이어도 header 는 길이 0 의 유효한 msg_t 로 전달된다.
       body 도 마찬가지. NULL 이 들어오지 않는다. */
    /* source_rid 는 콜백 실행 중에만 유효한 borrowed view 이므로,
       이후에도 유지하려면 값을 복사해 둔다. */

    /* ... header / body 로 상위 프로토콜 처리 ... */

    zlink_msg_close(header);
    zlink_msg_close(body);
}

zlink_stream_packet_handler(stream, on_packet, NULL);
```

패킷 콜백 모드의 규칙은 다음과 같다.

- `header_size` 와 `body_size` 는 각각 0 도 허용된다. 길이가 0 이어도 msg_t 는
  유효한 객체로 전달된다.
- `header` 와 `body` 의 소유권은 콜백으로 이전된다. 콜백은 두 msg_t 를 각각
  정확히 한 번 close 하거나 소비해야 한다.
- 같은 핸들에서 직접 수신 모드(`zlink_recv()`), 콜백 수신 모드
  (`zlink_recv_handler()`), 데이터 경로 `ZLINK_POLLIN` 등록은 모두
  `EBUSY` 로 실패한다. 두 번째 패킷 핸들러 등록도 마찬가지다.
- framing 규약을 지키지 않는 비정형 패킷(malformed packet)(길이 제한 초과, 조립 실패,
  불완전 상태 연결 종료 등)은 연결을 닫는 기본 동작으로 이어진다. 이
  이벤트는 소켓 모니터(socket monitor) 경로로 관찰한다.

이 모드는 조각 누적을 응용 쪽에서 다시 구현하지 않아도 되는 편의를
주지만 transport 조각 경계와 패킷 경계가 다르다는 점 자체를 바꾸지는 않는다.

---

## 5. 클라이언트 구현 원칙

클라이언트는 raw socket/websocket로 구현한다.
STREAM은 raw 바이트를 그대로 전달하므로 **패킷 경계(framing)는 애플리케이션이 정의**해야 한다.

개념적 POSIX TCP 예시 (RAW 모드 — zlink framing 없음, 바이트 그대로):

```c
// RAW 모드: raw 바이트 송수신. 메시지 경계는 애플리케이션이 정의
send(fd, body, body_len, 0);

char buf[4096];
ssize_t n = recv(fd, buf, sizeof(buf), 0);
```

서버가 **패킷 핸들러**(`zlink_stream_packet_handler`)를 쓰면 클라이언트는 각
패킷을 2바이트 BE header size + 4바이트 BE body size + header + body로
framing해야 한다:

```c
// 패킷 모드: [2B header_size BE][4B body_size BE][header][body]
uint16_t hsz_be = htons(header_len);
uint32_t bsz_be = htonl(body_len);
send(fd, &hsz_be, 2, 0);
send(fd, &bsz_be, 4, 0);
send(fd, header, header_len, 0);
send(fd, body, body_len, 0);
```

---

## 6. 옵션 및 런타임 정책

주요 옵션:

- 지원:
  - `ZLINK_OPT_MAXMSGSIZE`
  - `ZLINK_OPT_SNDHWM` / `ZLINK_OPT_RCVHWM`
  - `ZLINK_OPT_SNDBUF` / `ZLINK_OPT_RCVBUF`
  - `ZLINK_OPT_BACKLOG`
  - `ZLINK_OPT_LINGER`
  - `ZLINK_STREAM_OPT_NOTIFY` (`zlink_set_stream_option()` / `zlink_get_stream_option()`): 현재는 옵션 값을 set/get만 한다(저장/조회)
- TLS/WSS 서버: `zlink_set_tls_server()`
- TLS 클라이언트: `zlink_set_tls_client()`

STREAM listener는 raw TCP 피어가 보낸 바이트를 직접 받을 수 있다. 피어를 완전히 신뢰할 수 없다면
`zlink_bind`를 호출하기 전에 애플리케이션이 받아들일 최대 메시지 크기로
`ZLINK_OPT_MAXMSGSIZE`를 설정한다. 이 값을 설정하지 않으면 호환성을 위해 기본값은 무제한이다.

비지원/변경:
- `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID`를 STREAM에 설정하면 `EOPNOTSUPP`

### 6.1 STREAM 기본 런타임 프로파일

현재 STREAM 내부 기본값:
- `ZLINK_OPT_BACKLOG`: `65536`
- `ZLINK_OPT_SNDHWM` / `ZLINK_OPT_RCVHWM`: 기본 balanced auto-HWM 정책의 STREAM profile byte 값. context auto-HWM을 끄면 수동 byte 기본값 사용
- `ZLINK_OPT_SNDBUF` / `ZLINK_OPT_RCVBUF`: 기본값 `-1`. OS 기본 버퍼와 TCP 자동 조정에 맡김
- STREAM 배치 크기 기본값: `4096`
- STREAM 읽기 여유 공간 기본값: `64`
- STREAM accept 동시성 기본값: `4` (최대 `128`로 제한)
- STREAM 세션 스케줄링 기본값: `rr`

> STREAM 런타임 환경변수 및 내부 튜닝 상수는
> [STREAM 내부 문서](../internals/stream-socket.ko.md)를 참고.

---

## 7. 에러/제약

- `zlink_connect(stream, ...)` -> `EOPNOTSUPP`
- STREAM 대상 `routing_id`는 4바이트여야 하며, 크기가 다르면 호출자 인자 오류(`EINVAL`)
- `MAXMSGSIZE` 초과 메시지는 연결 종료(disconnect 이벤트)

---

## 8. 테스트 기준 구현

참고 파일:
- `core/tests/integration/test_stream_socket.cpp`
- `core/tests/integration/test_stream_fastpath.cpp`
- `core/tests/integration/routing-id/test_connect_rid_string_alias.cpp`
- `core/tests/scenario/stream/zlink/test_scenario_stream_zlink.cpp`

위 테스트들은 STREAM 서버 + raw client 경로를 기준으로 동작한다.

---
[← ROUTER](03-4-router.ko.md) | [Proxy →](03-6-proxy.ko.md) | [Transport →](04-transports.ko.md)

## 언어별 완전한 예제

STREAM 소켓으로 원시 바이트를 주고받는 자립형 예제다(모든 바인딩, 빌드·실행 검증됨).

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/stream_recv_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/StreamRecv/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/StreamRecvSample.java:doc"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "bindings/kotlin/samples/src/main/kotlin/systems/zlink/samples/StreamRecvSample.kt:doc"
    ```

=== "Python"

    ```python
    --8<-- "bindings/python/samples/stream_recv_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/stream_recv_sample.ts:doc"
    ```

=== "JavaScript"

    ```javascript
    --8<-- "bindings/javascript/samples/stream_recv_sample.js:doc"
    ```

=== "Go"

    ```go
    --8<-- "bindings/go/samples/stream_recv_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/stream_recv_sample.rs:doc"
    ```

### 패킷 콜백 방식

수신 패킷을 콜백으로 처리하는 변형이다.

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/stream_packet_callback_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/StreamPacketCallback/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/StreamPacketCallbackSample.java:doc"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "bindings/kotlin/samples/src/main/kotlin/systems/zlink/samples/StreamPacketCallbackSample.kt:doc"
    ```

=== "Python"

    ```python
    --8<-- "bindings/python/samples/stream_packet_callback_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/stream_packet_callback_sample.ts:doc"
    ```

=== "JavaScript"

    ```javascript
    --8<-- "bindings/javascript/samples/stream_packet_callback_sample.js:doc"
    ```

=== "Go"

    ```go
    --8<-- "bindings/go/samples/stream_packet_callback_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/stream_packet_callback_sample.rs:doc"
    ```

---
<!-- zlink-nav:bottom:start -->
[← ROUTER](03-4-router.ko.md) | [프록시 →](03-6-proxy.ko.md)
<!-- zlink-nav:bottom:end -->
