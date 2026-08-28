---
title: "PAIR 소켓"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Routing ID](08-routing-id.ko.md) | [다음: PUB/SUB](03-2-pubsub.ko.md)
<!-- zlink-nav:end -->

# PAIR 소켓

> **이 장의 계약 소유 문서** — [PAIR socket 스펙](../spec/core/socket/01-pair.ko.md)이
> 다룬다. 이 챕터는 그 계약을 언어별 예제로 보여준다.

## 1. 개요

PAIR 소켓은 정확히 하나의 peer와 1:1 양방향 독점 연결을 맺는다. 두 번째 peer가 연결하면 그 나중 연결이 거부되고, 첫 번째 peer가 pipe를 유지한다.

**핵심 특성:**
- 단일 파이프만 허용 (1:1 독점)
- 양방향 자유 메시징 (send/recv 순서 무관)
- 가장 단순한 소켓 타입

**유효한 소켓 조합:** PAIR ↔ PAIR

```mermaid
flowchart LR
    A[PAIR A] <-->|Bidirectional| B[PAIR B]
```

## 2. 기본 사용법

### 생성 및 연결

```c
void *ctx = zlink_ctx_new();

/* Server side */
void *server = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(server, "tcp://*:5555");

/* Client side */
void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, "tcp://127.0.0.1:5555");
```

### 메시지 교환

PAIR의 공개 수신 API는 recv/poller 전용이다. `zlink_recv_part()`로 한 번에 part
하나씩 수신하며, 보통 poller 루프 안에서 호출한다. 양쪽 모두 send와 recv를
자유롭게 호출할 수 있다. PAIR는 peer가 정확히 하나이므로 선택할 routing id가
없다 — `source_rid_out_`에는 `NULL`을 넘긴다.

```c
/* Client → Server */
zlink_msg_t msg;
zlink_msg_init_size(&msg, 5);
memcpy(zlink_msg_data(&msg), "Hello", 5);
zlink_send_part(client, &msg, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);

/* Server receives with zlink_recv_part() (typically inside a poller loop) */
zlink_msg_t part;
zlink_part_flag_t more;
zlink_msg_init(&part);
if (zlink_recv_part(server, NULL, &part, &more, ZLINK_RECV_FLAGS_NONE) == ZLINK_RECV_OK) {
    printf("Received: %.*s\n",
           (int)zlink_msg_size(&part),
           (char *)zlink_msg_data(&part));
    zlink_msg_close(&part);
}

/* Server → Client (bidirectional; client uses the same recv+poller pattern) */
zlink_msg_t reply;
zlink_msg_init_size(&reply, 5);
memcpy(zlink_msg_data(&reply), "World", 5);
zlink_send_part(server, &reply, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

### 멀티파트 데이터 전송

멀티파트 데이터는 `zlink_send_part()`를 part마다 한 번씩 호출해서 전송한다.
마지막 part를 제외한 모든 part는 `ZLINK_PART_MORE`를, 마지막 part는
`ZLINK_PART_FINAL`을 사용한다. core는 최종 part가 성공할 때까지 중간 part들을
하나의 record로 스테이징한다.

```c
zlink_msg_t part0, part1;
zlink_msg_init_size(&part0, 3);
memcpy(zlink_msg_data(&part0), "foo", 3);
zlink_msg_init_size(&part1, 6);
memcpy(zlink_msg_data(&part1), "foobar", 6);

zlink_submit_result_t rc = zlink_send_part(
    server, &part0, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE);
if (rc == ZLINK_SUBMIT_OK)
    rc = zlink_send_part(server, &part1, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);

/* 수신자는 zlink_recv_part()를 개별 호출로 반복하며 프레임을 하나씩 받는다.
   has_more_out_ == ZLINK_PART_FINAL이 될 때까지 반복:
   part 1 = "foo" (more == ZLINK_PART_MORE), part 2 = "foobar" (more == ZLINK_PART_FINAL) */
```

> 참고: `core/tests/integration/test_public_inproc_multipart_send.cpp` — `test_public_inproc_pair_send_multipart_blocking()` 테스트

### 수신 모드

PAIR의 공개 수신 API는 recv/poller 전용이다.
`zlink_recv_part()`로 part 하나를 동기 수신한다.

```c
void *pair = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(pair, "tcp://*:5556");

zlink_msg_t part;
zlink_part_flag_t more;
zlink_msg_init(&part);
zlink_recv_result_t rc = zlink_recv_part(
    pair, NULL, &part, &more, ZLINK_RECV_FLAGS_NONE);
if (rc == ZLINK_RECV_OK) {
    /* part를 처리한다; more == ZLINK_PART_MORE면 같은 record의
       다음 part가 이어진다 */
    zlink_msg_close(&part);
}
```

> HWM(High-Water Mark, queue가 보관할 수 있는 accounted byte 상한) 도달 시 `zlink_send_part()`는 대기(기본) 또는 `ZLINK_DONTWAIT`로
> `ZLINK_SUBMIT_BACKPRESSURED`를 반환한다. 고급 배압(backpressure) 패턴은
> [socket option 가이드](12-socket-options.ko.md)를 참고.

## 3. 메시지 형식

PAIR 소켓의 메시지 프레임에는 **애플리케이션 데이터만** 들어간다.

```
Single frame:     [data]
Multipart frame:  [frame1][frame2]...[frameN]
```

> `source_rid` 등 공통 수신 인터페이스는
> [소켓 패턴 개요](03-0-socket-patterns.ko.md#공통-수신-방식)를 참고.

멀티파트 전송:

```c
zlink_msg_t header, body;
zlink_msg_init_size(&header, 6);
memcpy(zlink_msg_data(&header), "header", 6);
zlink_msg_init_size(&body, 4);
memcpy(zlink_msg_data(&body), "body", 4);

zlink_submit_result_t rc = zlink_send_part(
    server, &header, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE);
if (rc == ZLINK_SUBMIT_OK)
    rc = zlink_send_part(server, &body, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

## 4. 소켓 옵션

| 옵션 | 타입 | 기본값 | 설명 |
|------|------|--------|------|
| `ZLINK_OPT_SNDHWM` | `uint64_t` bytes | 자동 | PAIR의 peer-queue 역할에 맞춰 계산한 자동 HWM. 수동 설정이 우선하며 `0`은 무제한 |
| `ZLINK_OPT_RCVHWM` | `uint64_t` bytes | 자동 | PAIR의 peer-queue 역할에 맞춰 계산한 자동 HWM. 수동 설정이 우선하며 `0`은 무제한 |
| `ZLINK_OPT_LINGER` | int | -1 | close 시 미전송 메시지 대기 시간 (ms), -1=무한 |
| `ZLINK_OPT_SNDTIMEO` | int | 1000 | 송신 타임아웃(ms). 무한 대기는 `-1`을 명시적으로 설정 |
| `ZLINK_OPT_RCVTIMEO` | int | 1000 | 수신 타임아웃(ms). 무한 대기는 `-1`을 명시적으로 설정 |

```c
uint64_t hwm_bytes = 5 * 1024 * 1024;  /* HWM은 byte이고 정확히 8 byte로 전달한다 */
zlink_set_option(socket, ZLINK_OPT_SNDHWM, &hwm_bytes, sizeof(hwm_bytes));

int linger = 0;  /* return immediately on close */
zlink_set_option(socket, ZLINK_OPT_LINGER, &linger, sizeof(linger));
```

## 5. 사용 패턴

### 패턴 1: 스레드 간 시그널링 (inproc)

가장 일반적인 PAIR 사용 사례. inproc transport로 스레드 간 제로카피(zero-copy, 메모리 복사 없이 전달) 통신.

```c
/* Main thread */
void *signal = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(signal, "inproc://signal");

/* Worker thread */
void *worker_signal = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(worker_signal, "inproc://signal");

/* Worker → Main: task completion signal */
zlink_msg_t msg;
zlink_msg_init_size(&msg, 4);
memcpy(zlink_msg_data(&msg), "DONE", 4);
zlink_send_part(worker_signal, &msg, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);

/* Main: poller 루프(zlink_recv_part)로 "DONE" 수신 */
```

> 참고: `core/tests/integration/test_pair_inproc.cpp` — bind → connect → bounce 패턴

### 패턴 2: TCP 통신

네트워크를 통한 1:1 통신. 와일드카드 바인드로 포트를 자동 할당할 수 있다.

```c
/* Server: wildcard port */
void *server = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(server, "tcp://127.0.0.1:*");

/* Query the assigned endpoint */
char endpoint[256];
size_t len = sizeof(endpoint);
zlink_get_option(server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len);

/* Client: connect using the queried endpoint */
void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, endpoint);
```

> 참고: `core/tests/integration/test_pair_tcp.cpp` — `bind_loopback_ipv4()` + 와일드카드 바인드

### 패턴 3: DNS 이름 연결

호스트명으로도 연결할 수 있다.

```c
void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, "tcp://localhost:5555");
```

> 참고: `core/tests/integration/test_pair_tcp.cpp` — `test_pair_tcp_connect_by_name()`

### 패턴 4: IPC 통신

같은 머신의 프로세스 간 통신 (Linux/macOS).

```c
void *server = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_bind(server, "ipc:///tmp/myapp.ipc");

void *client = zlink_socket(ctx, ZLINK_SOCKET_PAIR);
zlink_connect(client, "ipc:///tmp/myapp.ipc");
```

> 참고: `core/tests/integration/test_pair_ipc.cpp` — IPC 경로 길이 검증 포함

## 6. 주의사항

### 단일 peer만 허용

PAIR 소켓은 하나의 연결만 유지한다. 두 번째 peer가 연결하면 그 나중 연결이 거부되고, 첫 번째 peer가 pipe를 유지한다.

```
 Allowed:  PAIR A ↔ PAIR B      (1:1)
 Invalid:  PAIR A ← PAIR B      (N:1 attempt: later peers rejected)
               ← PAIR C
```

N:1 통신이 필요하면 DEALER/ROUTER를 사용한다.

### inproc connect/bind 순서

inproc transport는 보통 bind를 먼저 호출하지만, connect를 먼저 해도 된다 — bind
이전의 connect는 pending connection으로 보관되었다가 bind 시점에 연결된다.

```c
/* 권장 순서 */
zlink_bind(a, "inproc://signal");     /* 1. bind */
zlink_connect(b, "inproc://signal");  /* 2. connect */

/* connect-before-bind 도 동작 — connect는 pending으로 보관됐다가 bind에서 연결됨 */
zlink_connect(b, "inproc://signal");  /* pending connection으로 보관 */
zlink_bind(a, "inproc://signal");     /* 이 시점에 b의 pending connect가 연결됨 */
```

### IPC 경로 길이

IPC endpoint의 파일 경로는 시스템 제한(보통 108자)을 넘을 수 없다.

```c
/* Path too long → ENAMETOOLONG error */
zlink_bind(socket, "ipc:///very/long/path/.../endpoint.ipc");
```

> 참고: `core/tests/integration/test_pair_ipc.cpp` — `test_endpoint_too_long()`

### HWM 동작

peer가 연결되지 않았으면 PAIR 송신은 queue에 쌓이지 않고 곧바로 backpressure로 처리된다(`ZLINK_DONTWAIT`면 `ZLINK_SUBMIT_BACKPRESSURED`, 아니면 sndtimeo까지 대기). peer가 연결돼 있고 느릴 때는 HWM까지 queue에 쌓이고, HWM을 넘으면 `zlink_send_part()`가 대기(기본) 또는 `ZLINK_SUBMIT_BACKPRESSURED`(`ZLINK_DONTWAIT`)를 반환한다.

### LINGER 설정

`zlink_close()`를 호출할 때 미전송 메시지가 남아 있으면 LINGER 시간만큼 대기한다. 테스트나 빠른 종료가 필요한 경우:

```c
int linger = 0;
zlink_set_option(socket, ZLINK_OPT_LINGER, &linger, sizeof(linger));
```

---
[← 소켓 패턴](03-0-socket-patterns.ko.md) | [PUB/SUB →](03-2-pubsub.ko.md)

## 언어별 완전한 예제

PAIR 소켓으로 메시지를 주고받는 자립형 예제다(모든 바인딩, 빌드·실행 검증됨).

=== "C++"

    ```cpp
    --8<-- "bindings/cpp/samples/pair_recv_sample.cpp:doc"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "bindings/dotnet/samples/PairRecv/Program.cs:doc"
    ```

=== "Java"

    ```java
    --8<-- "bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/PairRecvSample.java:doc"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "bindings/kotlin/samples/src/main/kotlin/systems/zlink/samples/PairRecvSample.kt:doc"
    ```

=== "Python"

    ```python
    --8<-- "bindings/python/samples/pair_recv_sample.py:doc"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "bindings/node/samples/pair_recv_sample.ts:doc"
    ```

=== "JavaScript"

    ```javascript
    --8<-- "bindings/javascript/samples/pair_recv_sample.js:doc"
    ```

=== "Go"

    ```go
    --8<-- "bindings/go/samples/pair_recv_sample/main.go:doc"
    ```

=== "Rust"

    ```rust
    --8<-- "bindings/rust/samples/pair_recv_sample.rs:doc"
    ```

---
<!-- zlink-nav:bottom:start -->
[← 소켓 패턴](03-0-socket-patterns.ko.md) | [PUB/SUB →](03-2-pubsub.ko.md)
<!-- zlink-nav:bottom:end -->
