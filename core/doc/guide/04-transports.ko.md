---
title: "Transport 가이드"
---

[English](04-transports.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: 프록시 패턴](03-6-proxy.ko.md) | [다음: TLS와 WSS](05-tls-security.ko.md)
<!-- zlink-nav:end -->

# Transport 가이드

> **이 장이 답하는 것** — `tcp://`·`inproc://`·`tls://`·`wss://` 등 endpoint 형식과
> transport별 특성을 비교한다. 각 socket에 적용되는 정확한 조건은 socket 스펙이
> 소유한다.

## 1. Transport 종류

| Transport | URI 형식 | 예시 | 암호화 | 핸드셰이크 |
|-----------|----------|------|:------:|:----------:|
| tcp | `tcp://host:port` | `tcp://127.0.0.1:5555` | - | - |
| ipc | `ipc://path` | `ipc:///tmp/test.ipc` | - | - |
| inproc | `inproc://name` | `inproc://workers` | - | - |
| ws | `ws://host:port/path` | `ws://127.0.0.1:8080` | - | O |
| wss | `wss://host:port/path` | `wss://server:8443` | O | O |
| tls | `tls://host:port` | `tls://server:5555` | O | O |

### 소켓별 Transport 지원

| Transport | PAIR | PUB/SUB | DEALER | ROUTER | STREAM |
|-----------|:----:|:-------:|:------:|:------:|:------:|
| tcp       |  O   |    O    |   O    |   O    | O (bind) |
| ipc       |  O   |    O    |   O    |   O    | O (bind) |
| inproc    |  O   |    O    |   O    |   O    |   -    |
| tls       |  O   |    O    |   O    |   O    | O (bind) |
| ws        |  O   |    O    |   O    |   O    | O (bind) |
| wss       |  O   |    O    |   O    |   O    | O (bind) |

- STREAM은 **bind만** 지원하며, 클라이언트는 raw socket/websocket으로 구현한다.
- STREAM은 inproc을 지원하지 않는다(ipc는 bind만 지원).
- `tls`/`ws`/`wss`는 빌드 옵션에 따라 제공되며, `zlink_has("tls"|"ws"|"wss")`로 확인할 수 있다.

## 2. TCP

표준 TCP/IP 네트워크 통신.

### 기본 사용법

```c
/* Server: specific interface */
zlink_bind(socket, "tcp://192.168.1.10:5555");

/* Server: all interfaces */
zlink_bind(socket, "tcp://*:5555");

/* Client: IP address */
zlink_connect(socket, "tcp://127.0.0.1:5555");

/* Client: DNS name */
zlink_connect(socket, "tcp://server.example.com:5555");
```

### 와일드카드 포트 (자동 할당)

OS가 사용 가능한 포트를 자동으로 할당한다. 테스트나 동적 포트 환경에서 쓰기 좋다.

```c
/* Use port 0 or * */
zlink_bind(socket, "tcp://127.0.0.1:*");

/* Query the assigned endpoint */
char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);
/* endpoint = "tcp://127.0.0.1:53821" (example) */

/* Connect using the retrieved endpoint */
zlink_connect(other_socket, endpoint);
```

> 참고: `core/tests/integration/test_pair_tcp.cpp` — `bind_loopback_ipv4()` 와일드카드 바인드 패턴

### DNS 이름 사용

connect 시 호스트명을 쓰면 내부적으로 DNS를 resolve한다.

```c
/* Connect using DNS name */
zlink_connect(socket, "tcp://localhost:5555");
```

> 주의: DNS resolve은 블로킹으로 동작한다. 프로덕션에서는 IP 주소를 권장한다.
> 참고: `core/tests/integration/test_pair_tcp.cpp` — `test_pair_tcp_connect_by_name()`

### 에러 처리

```c
/* bind 실패: 포트 이미 사용 중 */
zlink_bind_result_t bind_rc = zlink_bind(socket, "tcp://*:5555");
if (bind_rc == ZLINK_BIND_ADDR_IN_USE) {
    printf("Port 5555 already in use\n");
}

/* connect 실패: 잘못된 주소 */
zlink_connect_result_t conn_rc = zlink_connect(
    socket, "tcp://invalid:99999");
if (conn_rc != ZLINK_CONNECT_OK) {
    printf("Connection failed: %d\n", (int)conn_rc);
}
```

### 특성

- **TCP_NODELAY** 활성화 (Nagle 알고리즘 비활성화)
- **투기적 쓰기(speculative write)** — 동기 쓰기를 먼저 시도하고 실패하면 비동기로 전환
- **모아 쓰기(gather write)** — 헤더와 바디를 한 번에 보내 시스템 콜 횟수를 줄임

> 투기적 쓰기 등 내부 최적화 상세는 [architecture.md](../internals/architecture.ko.md)를 참고.

## 3. IPC

Unix 도메인 소켓 기반 로컬 프로세스 간 통신.

### 기본 사용법

```c
/* Server */
zlink_bind(socket, "ipc:///tmp/myapp.ipc");

/* Client */
zlink_connect(socket, "ipc:///tmp/myapp.ipc");
```

### 와일드카드 바인드

```c
/* IPC wildcard — auto-assigns a temporary path */
zlink_bind(socket, "ipc://*");

char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);
```

> 참고: `core/tests/integration/test_router_multiple_dealers.cpp` — `zlink_bind(router, "ipc://*")`

### 에러 처리

```c
/* 경로 너무 김 */
zlink_bind_result_t rc = zlink_bind(
    socket, "ipc:///very/long/path/.../endpoint.ipc");
if (rc == ZLINK_BIND_INTERNAL_ERROR) {
    /* IPC 경로가 플랫폼 sun_path 한계 이상 — ENAMETOOLONG */
    printf("IPC path exceeds platform sun_path limit\n");
}
```

> 참고: `core/tests/integration/test_pair_ipc.cpp` — `test_endpoint_too_long()`

### 특성

- **Linux/macOS에서만 지원** (Windows 미지원)
- TCP 대비 낮은 오버헤드 (네트워크 스택 우회)
- 파일 경로 기반 주소 (경로는 플랫폼 sun_path 한계보다 짧아야 함)

## 4. inproc

프로세스 내(in-process) 통신. 가장 빠른 transport.

### 기본 사용법

```c
/* bind must be called first */
zlink_bind(socket_a, "inproc://workers");
zlink_connect(socket_b, "inproc://workers");
```

### 에러 처리

```c
/* bind 없이 connect 시도 */
zlink_connect_result_t rc = zlink_connect(socket, "inproc://nonexistent");
if (rc != ZLINK_CONNECT_OK) {
    printf("No bind exists yet\n");
}
```

### 특성

- **동일 context 내에서만** 사용 가능
- **bind가 connect보다 먼저** 호출되어야 함
- 잠금 없는(lock-free) 파이프 직접 연결 (네트워크 없음)
- 가장 낮은 지연시간, 가장 높은 처리량

> 참고: `core/tests/integration/test_pair_inproc.cpp` — bind → connect → bounce 패턴

## 5. WebSocket (ws)

웹 브라우저 및 외부 클라이언트 연동.

### 기본 사용법

```c
/* Server */
zlink_bind(socket, "ws://*:8080");

/* Client */
zlink_connect(socket, "ws://server:8080");

/* Wildcard port */
zlink_bind(socket, "ws://127.0.0.1:*");
char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);
```

> 참고: `core/tests/integration/test_stream_socket.cpp` — `test_stream_ws_basic()`

### 특성

- RFC 6455 준수
- Beast 라이브러리 기반
- 바이너리 프레임 모드 (Opcode=0x02)
- 64KB write buffer

## 6. WebSocket + TLS (wss)

암호화된 WebSocket 통신.

### 기본 사용법

```c
/* Server */
zlink_set_tls_server(socket, cert_path, key_path, 0);
zlink_bind(socket, "wss://*:8443");

/* Client */
zlink_set_tls_client(socket, ca_path, "localhost", 0);
zlink_connect(socket, "wss://server:8443");
```

> 참고: `core/tests/integration/test_stream_socket.cpp` — `test_stream_wss_basic()`

### ws 대비 추가 설정

| 설정 | ws | wss |
|------|:--:|:---:|
| `zlink_set_tls_server()` (서버 cert+key) | - | 필수 |
| `zlink_set_tls_client()` (클라이언트 CA+hostname+trust) | - | 권장 |

## 7. TLS

네이티브 TLS 암호화 통신.

### 기본 사용법

```c
/* Server */
zlink_set_tls_server(socket, "/path/to/cert.pem", "/path/to/key.pem", 0);
zlink_bind(socket, "tls://*:5555");

/* Client */
zlink_set_tls_client(socket, "/path/to/ca.pem", "server", 1);
zlink_connect(socket, "tls://server:5555");
```

상세 TLS 설정은 [TLS 보안 가이드](05-tls-security.ko.md)를 참고.

## 8. Transport 제약사항

| 제약 | 설명 |
|------|------|
| STREAM | bind만 지원, inproc 미지원(ipc는 bind만) |
| inproc | bind가 connect보다 먼저 호출 필요 |
| ipc | Unix/Linux/macOS만 지원 (Windows 미지원) |
| inproc context | 동일 context 내에서만 사용 |
| IPC 경로 | Unix 도메인 소켓 경로는 플랫폼 sun_path 한계보다 짧아야 함 |

## 9. Transport 선택 가이드

| 사용 사례 | 추천 Transport | 비고 |
|-----------|---------------|------|
| 스레드 간 통신 | inproc | 최고 성능 |
| 로컬 프로세스 간 (Unix) | ipc | TCP 대비 낮은 오버헤드 |
| 로컬 프로세스 간 (Windows) | tcp | IPC 미지원 |
| 서버 간 통신 | tcp | 표준 네트워크 통신 |
| 암호화 통신 | tls | 네이티브 TLS |
| 웹 클라이언트 | ws 또는 wss | WebSocket |
| 최고 성능 순서 | inproc > ipc > tcp > ws | 오버헤드 증가 순 |

## 10. bind vs connect

### 기본 원칙

- **bind**: 안정적인 주소를 제공하는 쪽 (서버, 잘 알려진 주소)
- **connect**: 상대방 주소를 알고 연결하는 쪽 (클라이언트)

### 다중 bind/connect

하나의 소켓에 여러 엔드포인트를 bind하거나 connect할 수 있다.

```c
/* Multiple bind — listen on multiple interfaces */
zlink_bind(router, "tcp://192.168.1.10:5555");
zlink_bind(router, "tcp://10.0.0.1:5555");
zlink_bind(router, "ipc:///tmp/router.ipc");

/* Multiple connect — connect to multiple servers */
zlink_connect(dealer, "tcp://server1:5555");
zlink_connect(dealer, "tcp://server2:5555");
```

### ZLINK_OPT_LAST_ENDPOINT

와일드카드 바인드 후 실제 할당된 엔드포인트를 조회한다.

```c
zlink_bind(socket, "tcp://127.0.0.1:*");

char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);
printf("Bound endpoint: %s\n", endpoint);
```

성능 비교는 [성능 가이드](10-performance.ko.md)를 참고.

---
<!-- zlink-nav:bottom:start -->
[← 프록시](03-6-proxy.ko.md) | [TLS/보안 →](05-tls-security.ko.md)
<!-- zlink-nav:bottom:end -->
