# Java Stream Connector 사용 안내

Java Stream Connector는 JVM client가 ZLink Framework의 STREAM endpoint에 연결할 때 사용한다.
서버 framework와 독립된 `systems.zlink:zlink-stream-connector` 모듈이며, Spring Boot가 없는
도구·E2E client·봇에서도 사용할 수 있다.

공개 타입과 정확한 기본값은
[Java/Kotlin Stream Connector spec](../../../common/spec/stream-connector/languages/java/03-stream-connector.ko.md)이
정본이다. transport와 wire 동작은
[Stream Connector 공통 spec](../../../common/spec/stream-connector/32-stream-connector.ko.md)을 따른다.

## 1. 의존성 추가

사용 중인 ZLink 배포 버전을 `<version>`에 지정한다.

```kotlin
dependencies {
    // Java connector 본체와 TCP/TLS/WS/WSS transport를 제공한다.
    implementation("systems.zlink:zlink-stream-connector:<version>")
}
```

## 2. 연결과 종료

endpoint URI의 scheme이 transport를 결정한다. `tcp`, `tls`, `ws`, `wss`를 사용할 수 있다.
`createDefault(...)`는 `MANUAL` dispatch, heartbeat, 자동 reconnect와 JSON typed codec을 설정한다.

```java
import java.net.URI;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;

var options = ZLinkStreamConnectorOptions.createDefault(
    URI.create("tcp://127.0.0.1:19000"));

ZLinkStreamConnector connector =
    ZLinkStreamConnectorFactory.create(options);

// 연결 완료나 실패는 CompletionStage로 전달된다.
connector.connect().submit().toCompletableFuture().join();

try {
    // application 동작을 수행한다.
} finally {
    // close가 완료되면 같은 connector로 다시 connect할 수 없다.
    connector.close().submit().toCompletableFuture().join();
}
```

TLS와 WSS는 기본적으로 서버 인증서와 hostname을 검증한다. 인증서 검증을 생략하는 옵션은
자체 서명 인증서를 사용하는 테스트에서만 사용한다.

## 3. Typed message 송신과 request

기본 codec은 JSON이다. payload type에 `@ZLinkStreamPacketName`이 있으면 그 값을 packet name으로
사용하고, 없으면 class의 simple name을 사용한다. 호출별 `packetName(...)`을 지정하면 그 값이
우선한다.

```java
record LoginRequest(String userId) {}
record LoginReply(String sessionId) {}
record PresenceChanged(String userId, boolean online) {}

// one-way send의 stage는 송신 완료 또는 실패만 전달한다.
connector.send(new PresenceChanged("user-1", true))
    .metadata("operationId", "presence-user-1-online")
    .submit();

// request의 reply type은 submit에서 명시한다.
var login = connector.request(new LoginRequest("user-1"))
    .metadata("operationId", "login-20260730-001")
    .submit(LoginReply.class);

login.thenAccept(reply -> {
    // reply.sessionId()를 application 상태에 반영한다.
});
```

metadata에는 operation ID나 tracing 값처럼 작은 값을 넣는다. 큰 업무 데이터는 payload에 넣는다.
request timeout을 호출별로 바꾸려면 `timeout(Duration)`을 `submit(...)` 전에 호출한다.

## 4. 수신 message 처리

기본 `MANUAL` mode에서는 network receive가 handler를 직접 실행하지 않는다. application이 선택한
thread에서 주기적으로 `dispatch().submit()`을 호출해야 등록된 push handler, request completion,
disconnect와 error handler가 실행된다.

```java
var registration = connector.on(
    PresenceChanged.class,
    message -> {
        PresenceChanged payload = message.payload();
        // 이 callback은 application이 dispatch를 호출한 thread에서 실행된다.
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    });

// UI loop, game loop 또는 application scheduler에서 반복 호출한다.
connector.dispatch().submit();

// handler가 더 이상 필요하지 않으면 등록을 해제한다.
registration.close();
```

`IMMEDIATE` mode는 receive 경로에서 handler를 바로 실행한다. 별도의 dispatch 호출은 필요 없지만,
느린 handler가 receive를 지연시키므로 handler가 충분히 짧을 때만 사용한다.

특정 server push 하나를 기다릴 때는 `waitFor(...)`를 사용한다. 이 대기 기능은 아직 처리하지 않은
수신 message를 직접 소비하므로 `MANUAL` mode에서도 별도의 dispatch 호출이 필요하지 않다.

```java
var notice = connector.waitFor("MaintenanceNotice")
    .timeout(java.time.Duration.ofSeconds(5))
    .submit();
```

## 5. 운영 시 확인할 항목

- payload가 `maxSendPayloadSize`와 `maxReceivePayloadSize`를 넘지 않는지 확인한다.
- `MANUAL` mode에서는 dispatch 호출이 중단되지 않는지 확인한다.
- reconnect와 heartbeat 기본값을 바꿀 때는 장애 감지 시간과 재연결 부하를 함께 검토한다.
- 운영 TLS/WSS에서는 인증서 검증을 유지한다.
- process 종료 시 `close().submit()` 완료를 기다린다.

Kotlin coroutine과 `Flow`를 사용할 때는
[Kotlin Stream Connector 사용 안내](../../../kotlin/guide/stream-connector/README.ko.md)를 본다.
