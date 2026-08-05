---
title: "Go 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: Python](../python/index.ko.md) | [다음: Rust](../rust/index.ko.md)
<!-- bindings-nav:end -->

# Go 바인딩 가이드 (`zlink.systems/zlink/v11`)

> **이 장의 계약 소유 문서** — [Go bindings 스펙](../../spec/go/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

Go에서 zlink를 쓰는 방법을 실제 샘플 코드 중심으로 설명합니다.
메시징 개념의 깊은 설명은 [코어 가이드](https://kairos-code-dev.github.io/zlink/guide/01-overview/)가 다루며,
여기서는 Go API 표면에 집중합니다.

---

## 설치

**`zlink.systems/zlink/v11`** 모듈로 제공됩니다. 네이티브 코어는 플랫폼별로 함께 번들됩니다.

```bash
go get zlink.systems/zlink/v11
```

- **Go 1.25** 이상.
- 네이티브를 따로 설치할 필요는 없습니다 — RID별 `.so`/`.dll`을 자동으로 불러옵니다.

```go
import zlink "zlink.systems/zlink/v11"
```

---

## 5분 예제 — PING/ACK

`Pair` 소켓으로 한쪽이 `PING`을 보내면 다른 쪽이 `ACK`로 답하는 최소 예제입니다.
서버는 bind하고 클라이언트는 connect합니다.

```go
// 서버
ctx, _ := zlink.NewContext()
defer ctx.Close()

server, _ := ctx.PairSocket()
defer server.Close()

server.Bind("tcp://127.0.0.1:5555")

var received zlink.Received
if _, err := server.Recv(&received, zlink.RecvFlagsNone); err != nil { ... }
defer received.Close()

part, _ := received.SinglePartOrError()
fmt.Println(string(part.Data())) // PING

reply, _ := zlink.NewMessage([]byte("ACK"))
server.Send().Message(reply).Submit(nil)
```

```go
// 클라이언트
ctx, _ := zlink.NewContext()
defer ctx.Close()

client, _ := ctx.PairSocket()
defer client.Close()
client.Connect("tcp://127.0.0.1:5555")

ping, _ := zlink.NewMessage([]byte("PING"))
client.Send().Message(ping).Submit(nil)

var received zlink.Received
if _, err := client.Recv(&received, zlink.RecvFlagsNone); err != nil { ... }
defer received.Close()

part, _ := received.SinglePartOrError()
fmt.Println(string(part.Data())) // ACK
```

실제 코드에서는 반드시 에러를 확인합니다. 위 예제는 흐름을 보여주려고
`_`로 처리했습니다.

---

## 핵심 타입

모든 기능이 공유하는 4가지 기본 타입입니다.

### 1. 컨텍스트 (Context)

프로세스의 런타임 진입점입니다. 보통 하나만 만들어 두고 모든 소켓·서비스를 여기서
생성합니다.

```go
ctx, err := zlink.NewContext()
if err != nil { ... }
defer ctx.Close() // 컨텍스트를 닫으면 하위 소켓이 모두 종료됩니다
```

I/O 스레드 수를 조정하려면 `Options()`를 씁니다:

```go
opts := ctx.Options()
opts.SetIOThreads(4)
```

> 소켓은 컨텍스트가 닫히기 **전에** 명시적으로 닫기를 권장합니다.
> 컨텍스트를 닫으면 열려 있는 소켓의 블로킹 작업이 중단됩니다.
> ([스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) 참고)

### 2. 메시지 (Message)

페이로드 프레임 하나를 소유합니다. 전송하면 소유권이 넘어가므로 `Close()`를
따로 호출할 필요가 없습니다. 전송에 실패하면 소유권이 그대로 남으므로 재시도하거나
명시적으로 닫아야 합니다.

```go
// 바이트 슬라이스에서 복사본 생성
msg, err := zlink.NewMessage([]byte("payload"))

// 크기를 지정해 빈 프레임 할당, 직접 채워 넣기
msg, err := zlink.NewMessageWithSize(256)
copy(msg.Data(), myData)

// 문자열에서 복사본 생성
msg, err := zlink.NewMessageString("payload")

// 전송 안 하고 폐기할 때
defer msg.Close()
```

받은 메시지의 페이로드를 읽을 때는 `Data()`를 씁니다. 돌려받은 슬라이스는
메시지 수명에 묶여 있으니 필요하면 복사합니다:

```go
data := msg.Data()            // 메시지가 살아 있는 동안만 유효
snapshot := msg.Bytes()       // 독립 복사본
text := msg.Text()            // UTF-8 문자열 변환
```

### 3. Received — 수신 봉투

메시지를 받은 봉투입니다. 라우팅 ID, 파트 목록, 회신 컨텍스트(선택)를 담습니다.
미리 선언해 두고 여러 번 재사용할 수 있습니다. 파트는 `Close()`를 호출하면 해제됩니다.

```go
var received zlink.Received   // 스택에 선언, 힙 할당 없음
_, err = socket.Recv(&received, zlink.RecvFlagsNone)
defer received.Close()        // 수신된 파트 해제

// 단일 파트 접근
part, err := received.SinglePartOrError()
payload := part.Data()

// 멀티파트 접근
for _, part := range received.Parts() {
    _ = part.Data()
}

// 라우팅 ID (ROUTER/SPOT 수신 시)
rid := received.RoutingID() // *RoutingID, nil이면 없음
```

### 4. 라우팅 ID (RoutingID)

피어나 스팟을 식별하는 1~255 바이트의 불변 값입니다. 직접 만들거나
수신 봉투에서 꺼냅니다.

```go
rid := zlink.NewRoutingID([]byte("server-01"))
rid := zlink.NewRoutingIDString("server-01")
rid := zlink.NewRoutingIDUint32(1)          // 4바이트 big-endian
rid := zlink.NewRoutingIDUUIDBytes(uuid)    // 16바이트 UUID
rid, err := zlink.NewRoutingIDFromHex("0102...")   // 16진수 파싱

fmt.Println(rid.String())  // 사람이 읽기 좋은 형태로 출력
```

---

## 소유권과 수명

Go 바인딩의 소유권 규칙은 단순합니다.

| 상황 | 규칙 |
|------|------|
| `Submit` 성공 | 추가한 `*Message`의 소유권이 전송 스택으로 이전됩니다. `Close()` 불필요 |
| `Submit` 실패 | 소유권이 호출자에게 반환됩니다. `Close()` 필요 |
| `Recv` 성공 | 호출자가 `Received`의 소유권을 가집니다. `defer received.Close()` 필수 |
| `Request.SubmitAsync` 완료 | 회신 파트(`[]*Message`) 소유권이 호출자에게 옵니다. 각 파트를 `Close()` |
| `Context.Close()` | 컨텍스트 하위의 모든 블로킹 작업을 중단합니다 |

```go
// 패턴: 에러가 나도 안전하게
msg, _ := zlink.NewMessage([]byte("data"))
if _, err := socket.Send().Message(msg).Submit(nil); err != nil {
    defer msg.Close() // 전송 실패 시에만 닫음
}
// 전송 성공 시 msg는 이미 소비되어 Close() 불필요
```

---

## 에러 처리

Go 바인딩은 표준 `error` 인터페이스를 돌려줍니다. 결과 코드가 필요하면 타입
어서션으로 확인합니다.

```go
_, err := socket.Send().Message(msg).Submit(nil)
if err != nil {
    var submitErr *zlink.SubmitError
    if errors.As(err, &submitErr) {
        switch submitErr.Result {
        case zlink.SubmitBackpressured:
            // 백프레셔 — 잠시 후 재시도
        case zlink.SubmitNotConnected:
            // 연결된 피어 없음
        default:
            return err
        }
    }
    return err
}
```

에러 타입:

| 타입 | 설명 | Result 필드 |
|------|------|-------------|
| `*SubmitError` | 전송/발행 실패 | `SubmitResult` |
| `*RequestError` | 요청 실패 | `RequestResult` |
| `*RecvError` | 수신 실패 | `RecvResult` |
| `*BindError` | 바인드 실패 | `BindResult` |
| `*ConnectError` | 연결 실패 | `ConnectResult` |
| `*ConfigError` | 옵션 설정 실패 | `ConfigResult` |
| `*CloseError` | 닫기 실패 | `CloseResult` |
| `*HandlerError` | 핸들러 등록 실패 | `HandlerResult` |

논블로킹 수신에서 메시지가 없는 경우는 에러가 아닙니다:

```go
ok, err := socket.Recv(&received, zlink.RecvFlagsDontWait)
if err != nil { /* 진짜 에러 */ }
if !ok { /* 메시지 없음 */ }
```

---

## C API 대응표

| C API | Go API |
|-------|--------|
| `zlink_ctx_new()` | `zlink.NewContext()` |
| `zlink_ctx_term()` | `ctx.Close()` |
| `zlink_socket(ctx, type)` | `ctx.PairSocket()` 등 |
| `zlink_close(socket)` | `socket.Close()` |
| `zlink_bind(socket, ep)` | `socket.Bind(ep)` |
| `zlink_connect(socket, ep)` | `socket.Connect(ep)` |
| `zlink_send_part(...)` | `socket.Send().Message(m).Submit(nil)` |
| `zlink_recv_part(...)` | `socket.Recv(&received, flags)` |
| `zlink_msg_data(msg)` | `msg.Data()` |
| `zlink_msg_size(msg)` | `msg.Size()` |
| `zlink_msg_close(msg)` | `msg.Close()` |
| `zlink_routing_id_t` | `zlink.RoutingID` |
| `zlink_socket_monitor_open(...)` | `zlink.OpenSocketMonitor(socket, ...)` |
| `zlink_poller_new()` | `zlink.NewPoller()` |
| `zlink_timer_new()` | `zlink.NewTimer()` |

---

## 네이티브 라이브러리 / 배포

Go 바인딩은 플랫폼별 `.so`(Linux) 또는 `.dylib`(macOS)를 내장합니다. 별도 설치
없이 `go get`으로 쓸 수 있습니다.

사용 중인 네이티브 버전 확인:

```go
v := zlink.RuntimeVersion()
fmt.Printf("zlink %d.%d.%d\n", v.Major, v.Minor, v.Patch)
```

특정 기능을 지원하는지 확인:

```go
if zlink.Has("draft") {
    fmt.Println("draft API 지원")
}
```

스레딩: `Context`는 고루틴 사이에서 공유할 수 있지만, 소켓은 **하나의 고루틴에서만**
써야 합니다. ([스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) 참고)

---

## 샘플

`bindings/go/samples/` 에 있는 검증된 샘플 코드입니다.

| 샘플 | 설명 |
|------|------|
| `pair_recv_sample` | PAIR 소켓 송수신 |
| `dealer_router_recv_sample` | DEALER/ROUTER 송수신 |
| `request_reply_async_sample` | 비동기 요청/응답 |
| `pubsub_recv_sample` | XPUB/SUB 발행·구독 |
| `stream_recv_sample` | STREAM 원시 TCP |
| `stream_packet_callback_sample` | STREAM 패킷 콜백 |
| `monitor_recv_sample` | 모니터 이벤트 수신 |

> SPOT·Actor 예제는 core 바인딩이 아니라 framework 샘플이 다룬다. Go에는 아직
> framework 바인딩이 없다.

샘플 실행:

```bash
cd bindings/go
go run ./samples/pair_recv_sample/...
# 또는 전체 실행
./samples/run_samples.sh
```

---

## 더 보기

- **소켓 패턴**: [개요](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/) — [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/) · [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/) · [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/) · [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/) · [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/) · [프록시](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)
- **운영**: [소켓 옵션](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/) · [TLS](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/) · [모니터링](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/) · [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) · [메시지 API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/) · [라우팅 ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
