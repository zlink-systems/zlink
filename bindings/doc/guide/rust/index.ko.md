---
title: "Rust 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: Go](../go/index.ko.md)
<!-- bindings-nav:end -->

# Rust 바인딩 가이드 (`zlink`)

> **이 장의 계약 소유 문서** — [Rust bindings 스펙](../../spec/rust/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

Rust에서 zlink를 쓰는 방법을 실제 샘플 코드 중심으로 설명합니다.
메시징 개념은 [코어 가이드](https://kairos-code-dev.github.io/zlink/guide/01-overview/)를 참고하세요.

---

## 설치

`Cargo.toml`에 추가합니다.

```toml
[dependencies]
zlink = "11.2"
```

- **Rust 1.85** 이상 (edition 2024).
- 네이티브 코어가 빌드 시 함께 링크됩니다.

```rust
use zlink::{Context, Message, Received, RecvFlags};
```

---

## 5분 예제 — PING/ACK

```rust
use zlink::{Context, Message, Received, RecvFlags};

// 서버
let ctx = Context::new().unwrap();
let server = ctx.pair_socket().unwrap();
server.bind("tcp://127.0.0.1:5555").unwrap();

let mut received = Received::empty();
server.recv(&mut received, RecvFlags::NONE).unwrap();
println!("{}", received.parts()[0].as_str().unwrap()); // PING

let ack = Message::try_from(b"ACK").unwrap();
server.send().message(ack).submit().unwrap();
```

```rust
// 클라이언트
let ctx = Context::new().unwrap();
let client = ctx.pair_socket().unwrap();
client.connect("tcp://127.0.0.1:5555").unwrap();

let ping = Message::try_from(b"PING").unwrap();
client.send().message(ping).submit().unwrap();

let mut received = Received::empty();
client.recv(&mut received, RecvFlags::NONE).unwrap();
println!("{}", received.parts()[0].as_str().unwrap()); // ACK
```

---

## 핵심 타입

### 컨텍스트

```rust
let ctx = Context::new().expect("context creation failed");
// ctx가 drop되면 하위 소켓의 블로킹 작업이 중단됩니다
```

### 메시지

`Message`는 페이로드 프레임 하나를 소유합니다. `send`로 넘기면 소유권이
옮겨가(move) 이후 사용을 컴파일러가 막아줍니다.

```rust
// 바이트 슬라이스에서 생성
let msg = Message::try_from(b"payload").unwrap();

// 크기 지정 빈 프레임
let mut msg = Message::with_size(256).unwrap();
msg.data_mut().copy_from_slice(&data);

// 전송 — msg는 여기서 move됨
socket.send().message(msg).submit().unwrap();
// msg를 다시 쓰면 컴파일 에러 → 소유권 안전성을 타입으로 보장
```

수신된 메시지 읽기:

```rust
let part = &received.parts()[0];
let bytes: &[u8] = part.as_bytes();
let text: &str = part.as_str().unwrap();   // UTF-8
let size = part.size();
```

### Received — 수신 봉투

```rust
let mut received = Received::empty();   // 재사용 가능
socket.recv(&mut received, RecvFlags::NONE).unwrap();

let parts = received.parts();                       // &[Message]
let rid: Option<&RoutingId> = received.routing_id(); // ROUTER/SPOT
let seq: Option<u64> = received.request_seq();
```

### 라우팅 ID

```rust
let rid = RoutingId::from(b"server-01");
socket.set_routing_id(&rid).unwrap();
```

---

## 소유권과 수명

Rust의 소유권 시스템이 대부분을 컴파일 타임에 강제합니다.

| 상황 | 규칙 |
|------|------|
| `submit()` 성공 | `Message`가 이미 move됨 — 추가 처리 불필요 |
| `submit()` 실패 | `Result::Err` 반환, 빌더가 내부 상태 정리 |
| `recv()` | `&mut Received`로 in-place 수신, drop 시 파트 해제 |
| 비동기 요청 | 회신 `Vec<Message>` 소유, 각 `Message`는 drop으로 해제 |

```rust
// 에러 처리 패턴
let msg = Message::try_from(b"data").unwrap();
match socket.send().message(msg).submit() {
    Ok(_) => { /* 전송됨 */ }
    Err(e) => eprintln!("send failed: {e}"),
}
```

---

## 에러 처리

Rust 바인딩은 작업별 에러 타입을 `Result`로 돌려줍니다.

```rust
match socket.send().message(msg).submit() {
    Ok(_) => {}
    Err(e) => match e.code() {
        zlink::SubmitResult::Backpressured => { /* 재시도 */ }
        zlink::SubmitResult::NotConnected => { /* 연결 없음 */ }
        _ => return Err(e.into()),
    },
}
```

에러 타입: `SubmitError`, `RequestError`, `RecvError`, `BindError`,
`ConnectError`, `ConfigError`, `CloseError`, `HandlerError`.
각 타입은 `code()` 메서드로 결과 코드 enum을 노출합니다.

---

## C API ↔ Rust 대응표

| C API | Rust API |
|-------|----------|
| `zlink_ctx_new()` | `Context::new()` |
| `zlink_ctx_term()` | `drop(ctx)` |
| `zlink_socket(ctx, type)` | `ctx.pair_socket()` 등 |
| `zlink_bind(s, ep)` | `socket.bind(ep)` |
| `zlink_connect(s, ep)` | `socket.connect(ep)` |
| `zlink_send_part(...)` | `socket.send().message(m).submit()` |
| `zlink_recv_part(...)` | `socket.recv(&mut received, flags)` |
| `zlink_msg_data(msg)` | `part.as_bytes()` |
| `zlink_routing_id_t` | `RoutingId` |
| `zlink_socket_monitor_open(...)` | `SocketMonitor::open(&socket)` |
| `zlink_poller_new()` | `Poller::new()` |
| `zlink_timer_new()` | `Timer::new()` |

---

## 네이티브 라이브러리 / 배포

네이티브 코어는 빌드 시 자동으로 링크됩니다. 런타임 버전 확인:

```rust
let (major, minor, patch) = zlink::version();   // (i32, i32, i32) 튜플
println!("zlink {major}.{minor}.{patch}");
```

**스레딩 규칙:**

| 항목 | 규칙 |
|------|------|
| `Context` | `Sync` — 스레드 간 공유 가능 (`Arc<Context>`) |
| 소켓 | `Send`이지만 한 스레드에서만 사용. 동시 접근 금지 |
| `Message::as_bytes()` | 메시지 수명 동안만 유효 |

```rust
use std::sync::Arc;
let ctx = Arc::new(Context::new().unwrap());

let ctx2 = ctx.clone();
std::thread::spawn(move || {
    let socket = ctx2.dealer_socket().unwrap();
    // 이 스레드에서만 socket 사용
});
```

---

## 샘플

`bindings/rust/samples/` 디렉터리의 검증된 샘플입니다.

| 파일 | 설명 |
|------|------|
| `pair_recv_sample.rs` | PAIR 송수신 |
| `dealer_router_recv_sample.rs` | DEALER/ROUTER 송수신 |
| `request_reply_callback_sample.rs` | 콜백 요청/응답 |
| `pubsub_recv_sample.rs` | PUB/SUB 발행·구독 |
| `stream_recv_sample.rs` | STREAM 원시 TCP |
| `stream_packet_callback_sample.rs` | STREAM 패킷 콜백 |
| `monitor_recv_sample.rs` | 모니터 이벤트 수신 |

> SPOT·Actor 예제는 core 바인딩이 아니라 framework 샘플이 다룬다. Rust에는 아직
> framework 바인딩이 없다.

```bash
cd bindings/rust
cargo run --example pair_recv_sample
```

API 레퍼런스 생성:

```bash
cd bindings/rust
cargo doc --no-deps --open
```

---

## 더 보기

**소켓 패턴**
- [소켓 패턴 개요](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  — [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/) · [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/) · [프록시](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**서비스**
- [Framework 서비스 개요](../../../../framework/doc/framework/common/guide/server/03-concepts.ko.md)

**운영**
- [소켓 옵션](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/) · [TLS 보안](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/) · [모니터링](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/) · [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) · [메시지 API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/) · [라우팅 ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
