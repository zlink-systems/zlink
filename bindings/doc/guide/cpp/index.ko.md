---
title: "C++ 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: .NET](../dotnet/index.ko.md) | [다음: Java](../java/index.ko.md)
<!-- bindings-nav:end -->

# C++ 바인딩 가이드 (`zlink::`)

> **이 장의 계약 소유 문서** — [C++ bindings 스펙](../../spec/cpp/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

C 코어를 RAII 래퍼로 감싼 바인딩입니다(header-only는 아니며 컴파일 후 링크). C++에서 zlink를 쓰는 방법을 실제 샘플 코드 중심으로 설명합니다.
자세한 메시징 개념은 [코어 가이드](https://zlink-systems.github.io/zlink/ko/guide/01-overview/)가 다룹니다.

---

## 설치

C++ 바인딩은 CMake로 제공합니다.

```cmake
add_subdirectory(bindings/cpp)
target_link_libraries(my_app PRIVATE zlink::cpp)
```

- **C++20** 이상 (coroutine, concepts 사용).
- 네이티브 코어가 함께 링크됩니다.

```cpp
#include <zlink.hpp>   // 모든 공개 API
```

---

## 5분 예제

```cpp
#include <zlink.hpp>

// 서버
zlink::context_t ctx;
zlink::pair_socket_t server (ctx);
server.bind ("tcp://127.0.0.1:5555");

zlink::received_t inbound;
server.recv (inbound);
std::printf ("%s\n", inbound.parts ()[0].to_string ().c_str ()); // PING
inbound.close ();

zlink::message_t ack = zlink::message_t::from ("ACK");
server.send ().message (ack).submit ();
```

```cpp
// 클라이언트
zlink::context_t ctx;
zlink::pair_socket_t client (ctx);
client.connect ("tcp://127.0.0.1:5555");

zlink::message_t ping = zlink::message_t::from ("PING");
client.send ().message (ping).submit ();

zlink::received_t inbound;
client.recv (inbound);
std::printf ("%s\n", inbound.parts ()[0].to_string ().c_str ()); // ACK
inbound.close ();
```

---

## 핵심 타입

### 컨텍스트

`context_t`는 RAII로 관리합니다. 소멸자에서 자동으로 종료됩니다.

```cpp
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);
    // ...
} // ctx 소멸 시 하위 소켓의 블로킹 작업 중단
```

### 메시지

`message_t`는 페이로드 프레임 하나를 소유합니다. `send`로 전달하면 소유권이
이전(move)되고 이후 사용 시 무효 상태가 됩니다.

```cpp
// 문자열에서 생성
zlink::message_t msg = zlink::message_t::from ("payload");

// 바이트에서 생성
std::vector<uint8_t> bytes = {0x01, 0x02};
zlink::message_t msg = zlink::message_t::from (bytes);

// 크기 지정 빈 프레임
zlink::message_t msg = zlink::message_t::allocate (256);
std::memcpy (msg.data (), src, 256);

// 전송 — msg는 여기서 move됨
socket.send ().message (msg).submit ();
// 전송 후 msg는 무효 — 다시 쓰지 말 것
```

HWM 대기 가능 send에는 동기와 비동기 종결자가 모두 있습니다. plain thread에서는
Core의 blocking admission 경로를 따르는 `submit()`을 사용합니다. Coroutine에서는
DONTWAIT을 사용하고 socket completion queue에서 완료되는 `async()`를 `co_await`합니다.

```cpp
socket.send ().message (msg).submit (); // 동기, 기본은 HWM admission까지 blocking
co_await socket.send ().message (msg).async (); // 비동기, 호출 thread를 막지 않음
```

Request도 두 terminal style을 사용합니다. `submit()`은 reply까지 blocking하고,
`async()`는 socket completion queue에서 완료되는 awaitable을 반환합니다. Reply는 이
terminal의 결과이며 별도 DATA receive로 받지 않습니다.

접수된 pre-admission operation의 retry는 Core가 소유합니다. Caller retry queue를 만들거나
같은 payload를 다시 submit하지 않습니다. 공용 native
`ZLINK_OPT_PENDING_MAX_MSGS/BYTES` 제한은 pending SEND와 REQUEST에 함께 적용되며,
send 전용 pending option 이름은 없습니다. Completion은 local admission을 뜻할 뿐 peer
delivery나 application acknowledgement가 아닙니다.

소비하지 않은 `async_result_t`를 파괴하면 caller waiter만 detach됩니다. Core submit 전에는
Core를 호출하지 않고 language operation을 중단할 수 있지만, successful submit 뒤에는 Core가
계속 admission할 수 있고 socket owner가 늦게 온 completion을 drain합니다. STREAM은
bind/connect 전에 `stream_recv_mode_t::raw` 또는 `packet`을 고른 뒤 각각 `recv()` 또는
`recv_packet()`을 사용합니다.

public poller가 socket의 `poll_event_t::completion` owner이면 blocking request나 awaitable이
남아 있는 동안 다른 thread가 `wait()`를 계속 호출해야 합니다. Wait가 native completion을
drain해 binding state를 settle/cleanup하므로, 같은 thread가 wait 사이에서 blocking terminal을
호출하면 진행이 멈출 수 있습니다.

수신된 메시지 읽기:

```cpp
const zlink::message_t &part = inbound.parts ()[0];
std::string text = part.to_string ();              // 문자열 복사
std::span<const std::byte> bytes = part.bytes ();  // 뷰 (메시지 수명 동안만)
size_t size = part.size ();
```

### received_t — 수신 봉투

```cpp
zlink::received_t inbound;
int rc = socket.recv (inbound);   // 0 = 성공
// 또는 플래그 지정
socket.recv (inbound, zlink::recv_flags_t::none);

auto parts = inbound.parts ();                              // const vector
auto rid = inbound.routing_id ();                          // optional<routing_id_t>
auto token = inbound.reply_token ();                       // optional<reply_token_t>

inbound.close ();   // 명시적 해제 (또는 소멸자)
```

### 라우팅 ID

```cpp
auto rid = zlink::routing_id_t::from (
    reinterpret_cast<const uint8_t*> (text.data ()), text.size ());
socket.set_routing_id (rid);
```

---

## 소유권과 수명

| 상황 | 규칙 |
|------|------|
| `submit()` 성공 | `message_t`가 move됨 — 이후 사용 무효 |
| `async()` | Core completion 대기 동안 operation이 move된 message를 소유 |
| `submit()` 기타 실패 | 예외(`submit_error_t`) 발생, 메시지 소유권 유지 |
| `recv()` | `received_t&`로 in-place 수신, `close()` 또는 소멸자로 해제 |
| 비동기 요청 | 회신 `std::vector<message_t>` 소유, 벡터 소멸 시 자동 해제 |

```cpp
try {
    zlink::message_t msg = zlink::message_t::from ("data");
    socket.send ().message (msg).submit ();  // 성공 시 msg move
} catch (const zlink::submit_error_t &e) {
    // 전송 실패 처리
}
```

---

## 에러 처리

C++ 바인딩은 `zlink::binding_error_t`를 상속하는 작업별 예외를 던집니다.

```cpp
try {
    zlink::message_t msg = zlink::message_t::from ("data");
    socket.send ().message (msg).submit ();
} catch (const zlink::submit_error_t &e) {
    // blocking admission timeout/backpressure 결과도 포함
    // application policy를 적용하기 전에 e.result() 확인
}
```

예외 타입:

| 예외 | 발생 시점 | result() 타입 |
|------|----------|---------------|
| `submit_error_t` | 전송/발행 실패 | `submit_result_t` |
| `request_error_t` | 요청 실패 | `request_result_t` |
| `recv_error_t` | 수신 실패 | `recv_result_t` |
| `bind_error_t` | 바인드 실패 | `bind_result_t` |
| `connect_error_t` | 연결 실패 | `connect_result_t` |
| `config_error_t` | 옵션 설정 실패 | `config_result_t` |
| `close_error_t` | 닫기 실패 | `close_result_t` |
| `handler_error_t` | 핸들러 등록 실패 | `handler_result_t` |

모두 `binding_error_t`를 상속하며 `code()`, `internal_errno()`로 네이티브 코드를
확인할 수 있습니다. 일부 recv API는 예외 대신 `recv_result_t` 정수 코드를 반환합니다
(예제 참고).

---

## C API 대응표

| C API | C++ API |
|-------|---------|
| `zlink_ctx_new()` | `zlink::context_t{}` |
| `zlink_ctx_term()` | 소멸자 또는 `ctx.term()` |
| `zlink_socket(ctx, type)` | `zlink::pair_socket_t{ctx}` 등 |
| `zlink_bind(s, ep)` | `socket.bind(ep)` |
| `zlink_connect(s, ep)` | `socket.connect(ep)` |
| blocking `zlink_send_part(...)` / `zlink_send_part_rid(...)` | `socket.send().message(m).submit()` |
| DONTWAIT send + completion pull | `co_await socket.send().message(m).async()` |
| `zlink_recv_part(...)` | `socket.recv(received)` |
| `zlink_msg_data(msg)` | `part.data()` / `part.bytes()` |
| `zlink_msg_size(msg)` | `part.size()` |
| `zlink_routing_id_t` | `zlink::routing_id_t` |
| `zlink_socket_monitor_open(...)` | `socket.monitor_open(...)` |
| `zlink_poller_new()` | `zlink::poller_t{}` |
| `zlink_timer_new()` | `zlink::timer_t{}` |

---

## 네이티브 라이브러리 / 배포

```cpp
int major, minor, patch;
zlink::version (major, minor, patch);
std::printf ("zlink %d.%d.%d\n", major, minor, patch);

if (zlink::has ("draft")) {
    // draft API 지원
}
```

스레딩 규칙:

| 항목 | 규칙 |
|------|------|
| `context_t` | 스레드 간 공유 가능 |
| 소켓 | **하나의 스레드에서만** 사용. 동시 접근 금지 |
| Completion과 receive 전달 | caller-owned terminal 또는 pull loop에서 관찰 |
| `message_t::bytes()` | 메시지 수명 동안만 유효한 span |

flag 없는 동기 `submit()`은 HWM admission을 기다리는 동안 호출 thread를 멈춥니다.
plain thread에서는 그 thread만 대기합니다. 다른 작업을 계속해야 하는 coroutine에서는
`co_await async()`를 사용합니다. Managed send는 public DONTWAIT flag terminal을
노출하지 않습니다.

```cpp
// 올바른 패턴: 소켓 per-스레드
std::thread worker ([&ctx] {
    zlink::dealer_socket_t socket (ctx);
    socket.connect ("tcp://...");
    // 이 스레드에서만 socket 사용
});
```

---

## 샘플

`bindings/cpp/samples/` 디렉터리에 검증된 샘플이 있습니다.

| 파일 | 설명 |
|------|------|
| `pair_recv_sample.cpp` | PAIR 송수신 |
| `dealer_router_recv_sample.cpp` | DEALER/ROUTER 송수신(요청/응답) |
| `pubsub_recv_sample.cpp` | XPUB/SUB 발행·구독 |
| `stream_recv_sample.cpp` | STREAM 원시 TCP |
| `stream_packet_pull_sample.cpp` | STREAM PACKET pull |
| `monitor_recv_sample.cpp` | 모니터 이벤트 수신 |
| `request_reply_async_sample.cpp` | ROUTER/DEALER 비동기 요청/응답 |

> SPOT·Actor 예제는 core 바인딩이 아니라 framework C++ 샘플이 다룬다 —
> [Bingo](https://github.com/zlink-systems/zlink/tree/main/framework/languages/cpp/samples/Bingo) 등
> `framework/languages/cpp/samples/`를 본다.

```bash
cd bindings/cpp
# 샘플은 ZLINK_CPP_BUILD_SAMPLES=ON일 때만 빌드된다
cmake -B build -DZLINK_CPP_BUILD_SAMPLES=ON && cmake --build build
./build/sample_cpp_pair_recv_sample
# 또는 일괄 실행: ./samples/run_samples.sh
```

---

## 더 보기

- 소켓 패턴: [개요](https://zlink-systems.github.io/zlink/ko/guide/03-0-socket-patterns/) — [PAIR](https://zlink-systems.github.io/zlink/ko/guide/03-1-pair/) · [PUB/SUB](https://zlink-systems.github.io/zlink/ko/guide/03-2-pubsub/) · [DEALER](https://zlink-systems.github.io/zlink/ko/guide/03-3-dealer/) · [ROUTER](https://zlink-systems.github.io/zlink/ko/guide/03-4-router/) · [STREAM](https://zlink-systems.github.io/zlink/ko/guide/03-5-stream/) · [프록시](https://zlink-systems.github.io/zlink/ko/guide/03-6-proxy/)
- 운영: [소켓 옵션](https://zlink-systems.github.io/zlink/ko/guide/12-socket-options/) · [TLS](https://zlink-systems.github.io/zlink/ko/guide/05-tls-security/) · [모니터링](https://zlink-systems.github.io/zlink/ko/guide/06-monitoring/) · [스레드 안전성](https://zlink-systems.github.io/zlink/ko/guide/11-thread-safety/) · [메시지 API](https://zlink-systems.github.io/zlink/ko/guide/09-message-api/) · [라우팅 ID](https://zlink-systems.github.io/zlink/ko/guide/08-routing-id/)
