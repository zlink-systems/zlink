---
title: ".NET 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: 개요](../README.ko.md) | [다음: C++](../cpp/index.ko.md)
<!-- bindings-nav:end -->

# .NET 바인딩 가이드 (`Systems.Zlink`)

> **이 장의 계약 소유 문서** — [.NET bindings 스펙](../../spec/dotnet/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

**.NET(`Systems.Zlink`)에서 zlink를 사용하는 방법**을 설치·핵심 타입·소유권·에러 처리·배포까지 한 챕터로 정리합니다. 메시징 개념(소켓 패턴, 서비스, 운영)의 깊은 설명은 [더 보기](#더-보기)의 코어 가이드를 참고하세요.

---

## 설치

단일 NuGet 패키지 **`Systems.Zlink`** 로 제공되며 네이티브 코어가 함께 번들됩니다.

```bash
dotnet add package Systems.Zlink
```

- **.NET 8.0** 이상 (`net8.0`).
- 네이티브 설치 불필요 — RID별 바이너리를 자동 로드합니다.
  ([네이티브 라이브러리](#네이티브-라이브러리--배포) 참고)

```csharp
using Systems.Zlink;   // 모든 공개 API는 이 네임스페이스에 있습니다
```

---

## 5분 예제

`Pair` 소켓으로 한쪽이 `PING`을 보내고 다른 쪽이 `ACK`로 답하는 최소 예제입니다.
서버는 bind, 클라이언트는 connect 합니다.

```csharp
// 서버
using var ctx = Zlink.CreateContext();
using var server = ctx.CreatePairSocket();
using var mon = server.MonitorOpen(SocketEvent.ConnectionReady);
server.Bind("tcp://127.0.0.1:5555");
mon.Recv();   // 연결될 때까지 대기

using var received = Received.Create();
server.Recv(received);
Console.WriteLine(received.FirstPart().GetString());   // PING

using var reply = Message.From("ACK");
server.Send().Message(reply).Submit();
```

```csharp
// 클라이언트
using var ctx = Zlink.CreateContext();
using var client = ctx.CreatePairSocket();
using var mon = client.MonitorOpen(SocketEvent.ConnectionReady);
client.Connect("tcp://127.0.0.1:5555");
mon.Recv();

using var ping = Message.From("PING");
client.Send().Message(ping).Submit();

using var received = Received.Create();
client.Recv(received);
Console.WriteLine(received.FirstPart().GetString());   // ACK
```

---

## 핵심 타입

모든 기능이 공유하는 4가지 기본 타입입니다.

### 1. 컨텍스트 (Context)

프로세스의 런타임 진입점입니다. 보통 하나만 만들고 모든 소켓·서비스를 여기서
생성합니다.

```csharp
using var ctx = Zlink.CreateContext();
ctx.Options.IoThreads  = 4;     // I/O 스레드 수
ctx.Options.MaxSockets = 1024;  // 최대 소켓 수
// 옵션은 소켓을 만들기 전에 설정하세요.
```

`IContext`는 `IDisposable`/`IAsyncDisposable`입니다. 종료 시 `Shutdown()`으로
진행 중인 작업을 멈출 수 있고 `using`으로 자동 해제됩니다.

### 2. 메시지 (Message)

하나의 페이로드 프레임입니다. 문자열·바이트·미리 할당 버퍼로 만들 수 있습니다.

```csharp
byte[] buffer = GetPayload();

using var fromText  = Message.From("payload");      // 문자열(UTF-8)
using var fromBytes = Message.From(buffer);         // byte[] / ReadOnlySpan<byte> 복사
using var sized     = new Message(1024);            // 미리 할당 후 AsSpan()에 채움

int    size = fromText.Size;
string text = fromText.GetString();                  // UTF-8 디코딩
ReadOnlySpan<byte> view = fromText.AsReadOnlySpan();  // 복사 없이 읽기
byte[] copy             = fromText.ToArray();         // 복사해서 꺼내기
```

`Message`는 네이티브 저장소를 소유하므로 `IDisposable`입니다. `AsSpan()` /
`AsReadOnlySpan()`이 주는 span은 메시지가 살아있는 동안만 유효합니다. 메시지 모델
개념은 [메시지 API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/)를 참고하세요.

바인딩은 JSON, Protobuf, MessagePack 같은 객체 codec package를 제공하지 않는다.
이 계층은 raw `Message`와 byte payload를 주고받는 저수준 API만 유지한다.
객체 직렬화가 필요하면 framework codec extension을 framework 구성 단계에 등록한다.
framework의 actor join callback처럼 raw `Message`를 직접 주고받는 표면에서는
application 계층에서 명시적으로 byte payload를 만들고 해석한다.

### 3. 수신 (Received)

수신 결과를 담는 **재사용 가능한 봉투**입니다. 핫 패스에서 한 번 만들어
`Recv(...)` 루프에서 재사용하면 할당이 사라집니다.

```csharp
using var received = Received.Create();
socket.Recv(received);

Message      first = received.FirstPart();   // 첫 파트(소유권 이전 없음)
string       body  = first.GetString();
RoutingId?   from  = received.RoutingId;     // 라우팅 경로가 있으면
ulong?       seq   = received.RequestSeq;    // 요청/응답이면
IReadOnlyList<Message> parts = received.Parts;  // 멀티파트 전체
```

### 4. 라우팅 ID (RoutingId)

피어·스팟·액터를 식별하는 바이너리 안전 값 타입입니다. 정적 팩토리로만 만듭니다.
개념과 정책은 [라우팅 ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)를 참고하세요.

```csharp
RoutingId a = RoutingId.From("order-client");       // UTF-8 문자열
RoutingId b = RoutingId.From(0xC0FFEEu);             // uint32(빅엔디안)
RoutingId c = RoutingId.From(Guid.NewGuid());        // 16바이트 UUID
RoutingId d = RoutingId.FromHex("0a1b2c");           // 원시 hex
string    s = a.ToString();                          // 표시용 문자열
string    h = a.ToHex();                             // 원시 바이트 보존용
```

---

## 소유권과 수명

`IContext`·소켓·`Message`·`Received`는 모두 네이티브 리소스를 감싸며
`IDisposable`(및 대부분 `IAsyncDisposable`)을 구현합니다. **만든 것은 반드시
해제하세요** — 항상 `using`(또는 `await using`).

- 소켓은 그것을 만든 컨텍스트보다 **먼저** dispose 하세요.
- `Request().Async()`·`Join(...).Async()`가 반환하는 응답 파트
  (`IReadOnlyList<Message>`)는 **호출자 소유**입니다 — 사용 후 dispose 하세요.
- span을 보관하려면 `ToArray()`/`CopyTo(...)`로 복사하세요.

스레드 안전성 규칙은 [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)을 참고하세요.
`IContext`는 여러 스레드에서 공유해도 안전합니다. **소켓은 안전하지 않습니다** —
같은 소켓을 둘 이상의 스레드에서 동시에 호출하지 마세요.

---

## 에러 처리

하드 실패는 작업별 타입 예외로 나타납니다. 모두 `ZlinkException`을 상속하며
`Code`(정수 코드)와 작업별 `Result`(열거형)를 노출합니다.

```csharp
try
{
    socket.Bind("tcp://127.0.0.1:5555");
}
catch (ZlinkBindException ex) when (ex.Result == ZlinkBindException.ErrorCode.AddrInUse)
{
    Console.Error.WriteLine("포트가 이미 사용 중입니다.");
}
catch (ZlinkException ex)
{
    Console.Error.WriteLine($"zlink 오류 {ex.Code}: {ex.Message}");
    throw;
}
```

| 예외 | 발생 작업 |
|---|---|
| `ZlinkSubmitException` | 송신/발행 (`Submit`) |
| `ZlinkRequestException` | 요청/응답 (`Request`) — `TimedOut` 등 |
| `ZlinkRecvException` | 수신 (`Recv`) |
| `ZlinkBindException` / `ZlinkConnectException` | 바인드/연결 |
| `ZlinkConfigException` | 옵션/설정 |
| `ZlinkCloseException` / `ZlinkHandlerException` | 종료/콜백 |

**데이터 없음·일시적 백프레셔는 예외가 아닙니다.** 논블로킹 수신은 `Recv(...)`가
`false`를, 논블로킹 송신은 `Submit()`이 `false`를 반환하는 것으로 구분하세요:

```csharp
if (!socket.Recv(received, RecvFlags.DontWait)) { /* 데이터 없음 */ }
if (!socket.Send().Message(m).Flags(SendFlags.DontWait).Submit()) { /* 백프레셔 */ }
```

---

## C API 대응표

C 코어(`zlink.h`)에서 넘어오거나 다른 언어 바인딩과 비교할 때 쓰는 압축 매핑입니다.
.NET은 raw 함수 대신 객체와 플루언트 빌더로 감싸므로 1:1은 아니지만 개념 단위로는
대응합니다. 전체 C 함수 목록은 [코어 C API 가이드](https://kairos-code-dev.github.io/zlink/guide/02-core-api/)를
참고하세요.

| 영역 | C API (`zlink_*`) | .NET |
|------|-------------------|------|
| 컨텍스트 | `zlink_ctx_new` / `zlink_ctx_term` | `Zlink.CreateContext()` / `IContext.Dispose()` |
| 컨텍스트 옵션 | `zlink_ctx_set` / `zlink_ctx_get` | `IContext.Options` (`IoThreads`, `MaxSockets`, …) |
| 소켓 생성 | `zlink_socket(ctx, TYPE)` | `ctx.Create<Type>Socket()` (`CreatePairSocket()` 등) |
| 바인드 / 연결 | `zlink_bind` / `zlink_connect` | `socket.Bind(...)` / `socket.Connect(...)` |
| 연결 해제 | `zlink_disconnect` / `zlink_disconnect_rid` | `socket.Disconnect(string)` / `socket.DisconnectRid(RoutingId)` |
| 소켓 옵션 | `zlink_set_option` / `zlink_get_option` | 소켓별 강타입 속성(`socket.Options`) |
| routing id | `zlink_set_routing_id` / `zlink_get_routing_id` | `socket.SetRoutingId(RoutingId)` / `socket.GetRoutingId()` |
| 메시지 생성 | `zlink_msg_init` / `_init_size` / `_init_data` | `new Message(size)` / `Message.From(...)` |
| 메시지 접근 | `zlink_msg_data` / `zlink_msg_size` | `Message.AsReadOnlySpan()` / `Message.Size` |
| 메시지 해제 | `zlink_msg_close` / `zlink_multipart_close` | `Message.Dispose()` / `Zlink.MultipartClose(parts)` |
| 송신 | `zlink_send_part` (+`_rid`) | `socket.Send().Message(...).Submit()` |
| 수신 | `zlink_recv_part` | `socket.Recv(Received)` |
| 요청 / 응답 | `zlink_dealer_request_part` / `zlink_router_reply_part` | `dealer.Request()....Async()` / `router.Reply(...)` |
| 구독 | `zlink_set_subscription` / `zlink_subscribe_part` | `socket.SetSubscription(...)` / `socket.Subscribe(TopicMessage)` |
| 모니터 | `zlink_socket_monitor_open` / `_recv` | `socket.MonitorOpen(...)` / `monitor.Recv()` |
| 폴러 / 타이머 | `zlink_poller_*` / `zlink_timer_*` | `Zlink.CreatePoller()` / `Zlink.CreateTimer()` |
| 프록시 | `zlink_proxy` / `zlink_proxy_steerable` | `Zlink.Proxy(...)` / `Zlink.ProxySteerable(...)` |

> **이름 규칙**: C의 `snake_case`는 .NET에서 `PascalCase`가 됩니다. C의 `*_part`
> 계열(멀티파트 substrate)은 .NET에서 플루언트 빌더의 `.Message(...)` 누적으로
> 표현됩니다 — public 모양은 언어 관례를 따르되 의미 계약은 동일합니다.

---

## 네이티브 라이브러리 / 배포

`Systems.Zlink`는 네이티브 코어를 `runtimes/<rid>/native` 아래 번들하므로 일반
빌드에서는 추가 설정이 필요 없습니다. 환경변수 `ZLINK_LIBRARY_PATH`로 로드 경로를
지정할 수 있습니다. **self-contained**/single-file/**Native AOT** 게시 시에는 대상
RID 자산이 출력에 포함되는지 확인하세요 (`dotnet publish -r <rid>`).

스레딩: `IContext`는 스레드 안전하며 여러 스레드에서 공유 가능합니다. 소켓은
단일 스레드 소유 — 전체 규칙은 [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) 참고.

---

## 샘플

`bindings/dotnet/samples/`에 기능별 실행 가능한 예제가 있습니다.

| 샘플 | 다루는 기능 |
|---|---|
| `PairRecv` | PAIR 송수신 |
| `DealerRouterRecv` | DEALER/ROUTER 라우팅 |
| `RequestReplyAsync` | 비동기 요청/응답 |
| `PubSubRecv` | PUB/SUB 토픽 |
| `MonitorRecv` | 소켓 모니터 |
| `StreamRecv`, `StreamPacketCallback` | STREAM + 패킷 콜백 |

> SPOT·Actor 예제는 core 바인딩이 아니라 framework 샘플이 다룬다 —
> [Spot](../../../../framework/doc/framework/common/guide/server/06-spot.ko.md) ·
> [Actor](../../../../framework/doc/framework/common/guide/server/07-actor-spot.ko.md) 가이드를 본다.

실행: `./samples/run_samples.sh` (또는 `run_samples.ps1`).

---

## 더 보기

**소켓 패턴**
- [소켓 패턴 개요](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  - [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/)
  - [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/)
  - [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/)
  - [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/)
  - [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/)
  - [프록시](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**서비스**
- [Framework 서비스 개요](../../../../framework/doc/framework/common/guide/server/03-concepts.ko.md)
  - [Spot](../../../../framework/doc/framework/common/guide/server/06-spot.ko.md)
  - [Actor](../../../../framework/doc/framework/common/guide/server/07-actor-spot.ko.md)

**운영**
- [소켓 옵션](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/)
- [TLS 보안](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/)
- [모니터링](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/)
- [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)
- [메시지 API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/)
- [라우팅 ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
