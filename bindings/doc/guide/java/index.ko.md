---
title: "Java 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: C++](../cpp/index.ko.md) | [다음: Node.js](../node/index.ko.md)
<!-- bindings-nav:end -->

# Java 바인딩 가이드 (`systems.zlink`)

> **이 장의 계약 소유 문서** — [Java bindings 스펙](../../spec/java/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

Java에서 zlink를 사용하는 방법을 실제 샘플 코드 중심으로 설명합니다.
메시징 개념의 깊은 설명은 [코어 가이드](https://kairos-code-dev.github.io/zlink/guide/01-overview/)가 소유하며, 이 가이드는 Java API 사용에 집중합니다.

---

## 설치

Gradle 또는 Maven으로 추가합니다. 네이티브 코어가 플랫폼별로 번들됩니다.

**Gradle (build.gradle):**

```groovy
dependencies {
    implementation 'systems.zlink:zlink:11.2.0'
}
```

**Maven (pom.xml):**

```xml
<dependency>
    <groupId>systems.zlink</groupId>
    <artifactId>zlink</artifactId>
    <version>11.2.0</version>
</dependency>
```

- **Java 22** 이상.
- 네이티브 별도 설치 불필요 — RID별 공유 라이브러리를 자동 로드합니다.

```java
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
```

---

## 5분 예제

`Pair` 소켓으로 한쪽이 `PING`을 보내고 다른 쪽이 `ACK`로 답하는 최소 예제입니다.
모든 리소스는 `try-with-resources`로 관리합니다.

```java
// 서버
try (Context ctx = Zlink.createContext();
     var server = ctx.createPairSocket()) {

    server.bind("tcp://127.0.0.1:5555");

    try (Received received = new Received()) {
        server.recv(received, RecvFlags.NONE);
        String text = received.firstPart().toUtf8String();
        System.out.println(text); // PING

        try (Message reply = Message.from("ACK")) {
            server.send().message(reply).submit();
        }
    }
}
```

```java
// 클라이언트
try (Context ctx = Zlink.createContext();
     var client = ctx.createPairSocket()) {

    client.connect("tcp://127.0.0.1:5555");

    try (Message ping = Message.from("PING")) {
        client.send().message(ping).submit();
    }

    try (Received received = new Received()) {
        client.recv(received, RecvFlags.NONE);
        System.out.println(received.firstPart().toUtf8String()); // ACK
    }
}
```

---

## 핵심 타입

모든 기능이 공유하는 4가지 기본 타입입니다.

### 1. 컨텍스트 (Context)

프로세스의 런타임 진입점입니다. `AutoCloseable`을 구현하므로 try-with-resources로
관리합니다. 컨텍스트를 닫으면 하위 소켓·서비스의 블로킹 작업이 중단됩니다.

```java
try (Context ctx = Zlink.createContext()) {
    // 소켓과 서비스를 여기서 생성합니다
    var socket = ctx.createPairSocket();
    // ...
} // ctx.close() 자동 호출 → 하위 소켓 종료
```

I/O 스레드 수 조정:

```java
ctx.options().ioThreads(4);
```

### 2. 메시지 (Message)

페이로드 프레임 하나를 소유합니다. `AutoCloseable`을 구현합니다.
전송하면 소유권이 이전되어 별도로 닫을 필요가 없습니다.
전송에 실패하면 소유권이 유지되므로 재시도하거나 명시적으로 닫아야 합니다.

```java
// 문자열에서 복사본 생성
try (Message msg = Message.from("payload")) {
    socket.send().message(msg).submit();
}
// submit 성공 시 msg는 이미 소비됨 — try 블록이 닫혀도 무방

// 바이트 배열에서 복사본 생성
try (Message msg = Message.from(bytes)) { ... }

// 크기 지정으로 빈 프레임 할당
try (Message msg = new Message(256)) {
    msg.mutableDataBuffer().put(data);
    socket.send().message(msg).submit();
}
```

직접 `recv(..., RecvFlags.NONE)`를 호출하면 현재 Java thread가 native recv에서
대기합니다. 이 표면은 low-level socket API입니다. 많은 session이나 handler를 처리하는
framework 경로에서는 이 호출을 handler thread에 직접 올리지 말고, `Poller`로 readiness를
기다린 뒤 ready socket에서 `RecvFlags.DONT_WAIT` recv를 수행합니다. application handler는
framework가 설정한 handler executor 뒤에서 실행합니다.

```java
try (Poller poller = Zlink.createPoller()) {
    poller.add(socket, 1L, PollEventFlags.POLLIN);
    PollEvents events = new PollEvents(16);

    int count = poller.wait(events, Duration.ofMillis(10));
    for (int i = 0; i < count; i++) {
        while (true) {
            Received received = new Received();
            if (!socket.recv(received, RecvFlags.DONT_WAIT)) {
                received.close();
                break;
            }
            handlerExecutor.execute(() -> {
                try (received) {
                    handle(received);
                }
            });
        }
    }
}
```

수신된 메시지 읽기:

```java
int size = msg.size();
String text = msg.toUtf8String();    // UTF-8 변환
byte[] data = msg.data();            // 바이트 배열 복사
ByteBuffer buf = msg.dataBuffer();   // 읽기 전용 뷰
```

### 3. Received — 수신 봉투

수신한 메시지 봉투입니다. 라우팅 ID, 파트 목록, 선택적 회신 컨텍스트를 담습니다.
재사용이 가능합니다. `AutoCloseable`을 구현합니다.

```java
try (Received received = new Received()) {
    socket.recv(received, RecvFlags.NONE);

    // 단일 파트 접근
    Message part = received.firstPart();         // 첫 번째 파트
    Message part = received.singlePartOrThrow();  // 파트가 정확히 하나여야 함

    // 멀티파트 접근
    List<Message> parts = received.parts();

    // 라우팅 ID (ROUTER/SPOT 수신 시)
    Optional<RoutingId> rid = received.getRoutingId();
}
```

### 4. 라우팅 ID (RoutingId)

피어나 스팟을 식별하는 1~255 바이트의 불변 값입니다.

```java
RoutingId rid = RoutingId.from("server-01".getBytes(StandardCharsets.UTF_8));
RoutingId rid = RoutingId.from("server-01");
```

---

## 소유권과 수명

Java 바인딩의 소유권 규칙입니다. try-with-resources를 기본 패턴으로 사용합니다.

| 상황 | 규칙 |
|------|------|
| `submit()` 성공 | 추가한 `Message`의 소유권이 전송 스택으로 이전됩니다. 별도 `close()` 불필요 |
| `submit()` 실패(예외) | 소유권이 호출자에게 유지됩니다. try-with-resources가 자동 처리 |
| `recv()` 성공 | 호출자가 `Received`의 소유권을 가집니다. try-with-resources 필수 |
| `submitAsync()` 완료 | 회신 `List<Message>`는 호출자 소유. `Message.closeAll(reply)` 필요 |
| `Context.close()` | 컨텍스트 하위의 모든 블로킹 작업을 중단합니다 |

```java
// 패턴: try-with-resources로 안전하게
try (Message msg = Message.from("data")) {
    boolean submitted = socket.send().message(msg).submit();
    // submitted=true면 msg가 소비됨, false면 백프레셔(DONT_WAIT일 때만)
} // submit이 예외를 던지면 try-with-resources가 msg를 닫음
```

---

## 에러 처리

Java 바인딩은 `ZlinkException` 계층 구조로 예외를 던집니다.

```java
try (Message msg = Message.from("data")) {
    socket.send().message(msg).submit();
} catch (ZlinkSubmitException e) {
    switch (e.getResult()) {
        case BACKPRESSURED -> { /* 잠시 후 재시도 */ }
        case NOT_CONNECTED -> { /* 연결된 피어 없음 */ }
        default -> throw e;
    }
}
```

예외 타입:

| 예외 클래스 | 발생 시점 | 결과 필드 |
|------------|----------|-----------|
| `ZlinkSubmitException` | 전송/발행 실패 | `getResult(): SubmitResult` |
| `ZlinkRequestException` | 요청 실패 | `getResult(): RequestResult` |
| `ZlinkRecvException` | 수신 실패 | `getResult(): RecvResult` |
| `ZlinkBindException` | 바인드 실패 | `getResult(): BindResult` |
| `ZlinkConnectException` | 연결 실패 | `getResult(): ConnectResult` |
| `ZlinkConfigException` | 옵션 설정 실패 | `getResult(): ConfigResult` |
| `ZlinkCloseException` | 닫기 실패 | `getResult(): CloseResult` |
| `ZlinkHandlerException` | 핸들러 등록 실패 | `getResult(): HandlerResult` |

모든 예외는 `ZlinkException`을 상속하며 `getCode()`와 `getInternalErrno()`로
네이티브 코드를 확인할 수 있습니다.

---

## C API 대응표

| C API | Java API |
|-------|----------|
| `zlink_ctx_new()` | `Zlink.createContext()` |
| `zlink_ctx_term()` | `ctx.close()` |
| `zlink_socket(ctx, type)` | `ctx.createPairSocket()` 등 |
| `zlink_close(socket)` | `socket.close()` |
| `zlink_bind(socket, ep)` | `socket.bind(ep)` |
| `zlink_connect(socket, ep)` | `socket.connect(ep)` |
| `zlink_send_part(...)` | `socket.send().message(m).submit()` |
| `zlink_recv_part(...)` | `socket.recv(received, flags)` |
| `zlink_msg_data(msg)` | `msg.data()` |
| `zlink_msg_size(msg)` | `msg.size()` |
| `zlink_msg_close(msg)` | `msg.close()` |
| `zlink_routing_id_t` | `RoutingId` |
| `zlink_socket_monitor_open(...)` | `socket.monitorOpen(...)` |
| `zlink_poller_new()` | `Zlink.createPoller()` |
| `zlink_timer_new()` | `Zlink.createTimer()` |

---

## 네이티브 라이브러리 / 배포

Java 바인딩은 플랫폼별 공유 라이브러리를 내장합니다. 별도 설치 없이 Gradle/Maven으로
추가하면 됩니다.

사용 중인 네이티브 버전 확인:

```java
int[] version = Zlink.version();
System.out.printf("zlink %d.%d.%d%n", version[0], version[1], version[2]);
```

특정 기능 지원 여부:

```java
if (Zlink.has("draft")) {
    System.out.println("draft API 지원");
}
```

**스레딩:** `Context`는 스레드 간 공유 가능하나, 소켓은 **하나의 스레드에서만** 사용해야 합니다.
디스패치 핸들러는 zlink 내부 워커 스레드에서 호출되므로 핸들러 안에서 오래 블록하지 않아야 합니다.
자세한 내용은 [스레드 안전성](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)을 참고하세요.

---

## 샘플

`bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/` 에 있는
검증된 샘플 코드입니다.

| 샘플 클래스 | 설명 |
|------------|------|
| `PairRecvSample` | PAIR 소켓 송수신 |
| `DealerRouterRecvSample` | DEALER/ROUTER 송수신 |
| `RequestReplyAsyncSample` | 비동기 요청/응답 |
| `PubSubRecvSample` | PUB/SUB 발행·구독 |
| `StreamRecvSample` | STREAM 원시 TCP |
| `StreamPacketCallbackSample` | STREAM 패킷 콜백 |
| `MonitorRecvSample` | 모니터 이벤트 수신 |

> SPOT·Actor 예제는 core 바인딩이 아니라 framework 샘플이 다룬다 — 아래
> [더 보기](#더-보기)의 Spot·Actor 링크를 본다.

샘플 빌드 및 실행:

```bash
cd bindings/java
./gradlew :samples:build
./gradlew :samples:run -PmainClass=systems.zlink.samples.PairRecvSample
```

---

## Kotlin

Kotlin은 **별도 네이티브 바인딩 없이 Java 바인딩(`systems.zlink.*`)을 그대로**
사용합니다. 위의 설치·핵심 타입·소유권·에러·대응표가 모두 동일하게 적용되고,
Kotlin 관용만 다릅니다.

- **의존성**: `systems.zlink:zlink`(위와 동일). Kotlin 플러그인은 **2.1.0**
  이상을 씁니다.
- **소유권**: `AutoCloseable`이므로 `try`/`finally` 대신 `use { }`로 정리합니다.

```kotlin
Zlink.createContext().use { ctx ->
    ctx.createPairSocket().use { socket ->
        socket.bind("tcp://127.0.0.1:5555")
        // ...
    }
}
```

- **콜백**: 핸들러는 Kotlin 람다로 그대로 넘깁니다 — `timer.onFire { _, n -> ... }`.
- **샘플**: `bindings/kotlin/samples/`(`.kt`)에 Java 샘플과 같은 canonical 세트가
  있습니다. Java gradle의 `:kotlin-samples` 서브프로젝트로 빌드·실행합니다.

```bash
cd bindings/java
./gradlew :kotlin-samples:runPairRecvSample --no-daemon
```

코어 가이드의 언어 탭에는 **Kotlin** 칸이 따로 있어 메시징·서비스 사용법을
Kotlin 코드로 바로 볼 수 있습니다.

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
