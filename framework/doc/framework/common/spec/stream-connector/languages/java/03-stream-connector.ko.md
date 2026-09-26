<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md) | [이전: Java STREAM](../../../server/languages/java/interfaces/stream-session.ko.md)
<!-- framework-adapter-nav:end -->

[Java spec 목차](../../../server/languages/java/README.ko.md)

[Java 묶음](../../../../../java/README.ko.md) | [STREAM](../../../server/languages/java/interfaces/stream-session.ko.md) | [Samples](../../../../../../../languages/java/samples/README.md)

# Java/Kotlin Stream Connector

> 이 문서는 [Stream Connector 공통 스펙](../../32-stream-connector.ko.md)의 **Java/Kotlin
> 투영**이다. transport·wire·생명주기·오류 의미는 공통 스펙이 소유하고, 이 문서는 그 의미가
> Java/Kotlin에서 갖는 **정확한 public 표면**을 고정한다.

## 1. 목표

Stream Connector는 server framework와 별도 모듈이다. 서버의 `ZLinkSession`이 받는
framework header 기반 STREAM packet을 외부 client가 같은 방식으로 만들고 해석하게
한다.

이 모듈은 Spring Boot server adapter, SPOT, Registry에 의존하지 않는다. transport,
codec, compression, reconnect, dispatch queue처럼 client 실행에 필요한 의존성만 가진다.

### 1.1 대상 실행 환경

**엔진 × 빌드 타깃별 담당 connector는 [공통 스펙 §2](../../32-stream-connector.ko.md)가 소유한다.**
그 배정에 따라 Java/Kotlin connector가 담당하는 것은 **JVM 애플리케이션**(서버 도구·E2E 테스트·
봇)이며, 게임 엔진과 브라우저는 담당하지 않는다.

대상이 하나뿐이라 이 배정이 Java/Kotlin 표면에 남기는 결과는 없다. 사용 안내는
[Java guide](../../../../../java/guide/stream-connector/README.ko.md)와
[Kotlin guide](../../../../../kotlin/guide/stream-connector/README.ko.md)에서 언어별 비동기 사용법만
나누어 설명한다.

## 2. 모듈

Java connector의 Maven 좌표는 `systems.zlink:zlink-stream-connector`다.

| 모듈 | 역할 |
|------|------|
| `zlink-stream-connector` | TCP/TLS/WS/WSS transport, frame codec, send/request, dispatch |
| `zlink-framework-kotlin` | coroutine, `Flow`, DSL extension |
| `zlink-framework-codec-protobuf` | framework/connector/http-client에서 공유하는 Protobuf codec extension |
| `zlink-framework-codec-msgpack` | framework/connector/http-client에서 공유하는 MessagePack codec extension |

JSON은 framework 기본 codec이다. Protobuf와 MessagePack payload는 connector 전용 package가
아니라 `zlink-framework-codec-protobuf`, `zlink-framework-codec-msgpack` framework codec
extension을 connector에도 적용해서 사용한다.

## 3. Public API

```java
public interface ZLinkStreamConnector {
    boolean isConnected();
    ZLinkStreamConnectionState state();
    ZLinkStreamConnectorOptions options();

    // 마지막 종료 사유. 한 번도 끊긴 적이 없으면 비어 있다(아래 "세션 종료 사유").
    Optional<ZLinkStreamCloseReason> closeReason();

    int pendingDispatchCount();
    int receivedCount(String name);

    // lifecycle 표면은 셋뿐이다. 수동 재연결은 connect()의 상태 전이이고,
    // 자동 재연결은 options가 담당한다(공통 스펙 32 §6).
    ZLinkStreamLifecycleCall connect();
    ZLinkStreamLifecycleCall close();
    ZLinkStreamLifecycleCall dispatch();

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);
    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);
    ZLinkTypedStreamSendCall send(Object payload);
    ZLinkTypedStreamSendCall send(String name, Object payload);
    ZLinkTypedStreamRequestCall request(Object payload);
    ZLinkTypedStreamRequestCall request(String name, Object payload);
    ZLinkStreamWaitCall waitFor(String name);
    ZLinkStreamWaitCall waitFor(Class<?> payloadType);
    ZLinkStreamExpectNoneCall expectNone(String name);
    ZLinkStreamExpectNoneCall expectNone(Class<?> payloadType);
    ZLinkStreamSequenceCall waitForSequence(String name);
    ZLinkStreamSequenceCall waitForSequence(Class<?> payloadType);

    AutoCloseable on(
        String name,
        ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler);
    <TPayload> AutoCloseable on(
        Class<TPayload> payloadType,
        ZLinkStreamMessageHandler<TPayload> handler);
    <TPayload> AutoCloseable on(
        String name,
        Class<TPayload> payloadType,
        ZLinkStreamMessageHandler<TPayload> handler);
    AutoCloseable onRequestSending(ZLinkStreamRequestSendingHandler handler);
    AutoCloseable onReplyReceived(ZLinkStreamReplyReceivedHandler handler);
    AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler);
    AutoCloseable onDisconnected(ZLinkStreamDisconnectedHandler handler);
    AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler);

    // 지금 bind되어 있는 Actor handle(공통 스펙 §5.6). application이 만들지 않는다.
    List<ZLinkStreamActor> actors();
    Optional<ZLinkStreamActor> actor(String actorId);
    AutoCloseable onActorBound(ZLinkStreamActorHandler handler);
    AutoCloseable onActorUnbound(ZLinkStreamActorHandler handler);
}

public interface ZLinkStreamActor {
    String actorId();
    boolean isBound();                          // unbound 통지 뒤 false

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);      // 이 Actor의 slot을 싣는다
    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);
    ZLinkTypedStreamSendCall send(Object payload);
    ZLinkTypedStreamSendCall send(String name, Object payload);
    ZLinkTypedStreamRequestCall request(Object payload);
    ZLinkTypedStreamRequestCall request(String name, Object payload);
    AutoCloseable on(String name, ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler); // 이 Actor가 상대인 message만
    <TPayload> AutoCloseable on(Class<TPayload> payloadType, ZLinkStreamMessageHandler<TPayload> handler);
    <TPayload> AutoCloseable on(
        String name,
        Class<TPayload> payloadType,
        ZLinkStreamMessageHandler<TPayload> handler);
}

@FunctionalInterface
public interface ZLinkStreamActorHandler {
    CompletionStage<Void> handle(ZLinkStreamActor actor);
}

public interface ZLinkStreamLifecycleCall {
    CompletionStage<Void> submit();
}

public final class ZLinkStreamConnectorFactory {
    public static ZLinkStreamConnector create(ZLinkStreamConnectorOptions options);
}
```

Java는 event를 `on...` registration으로 노출한다. .NET의 event와 의미는 같다.
**등록 해제는 반환된 `AutoCloseable`의 `close()`로 하며, 같은 값을 두 번 닫아도 오류로 처리하지
않는다**([공통 스펙 §7](../../32-stream-connector.ko.md#7-dispatch-모드)). push handler와
error·disconnect·connection state handler가 모두 같은 반환 타입을 사용한다.

**세션 종료 사유 (close reason).** 값 집합과 의미는
[공통 스펙 §6.2](../../32-stream-connector.ko.md#62-종료-사유)가 소유한다. Java는 이를 닫힌 enum
`ZLinkStreamCloseReason`(`CLIENT_CLOSE`, `IDLE_TIMEOUT`, `HEARTBEAT_TIMEOUT`, `SERVER_DRAIN`,
`PROTOCOL_ERROR`, `TRANSPORT_ERROR`)으로 표현한다. **읽기 표면은 connector의
`Optional<ZLinkStreamCloseReason> closeReason()` method다.** 한 번도 끊긴 적이 없으면
`Optional.empty()`를 반환한다. `ZLinkStreamDisconnectedHandler`가 받는 disconnect 이벤트의
`ZLinkStreamCloseReason closeReason()`은 이 읽기 표면에 추가하는 것이다.
`waitFor(...)`는 특정 packet name의 server push를 한 번 기다리는 call builder를 반환한다.
필요한 message만 고를 때는 builder의 `where(...)`를 사용한다. timeout이 지나면 반환된
`CompletionStage`가 timeout 실패로 끝난다. 별도 timeout을 지정하지 않으면 connector
options의 `waitTimeout()` 값을 사용한다. `waitFor(...)`는 두 dispatch mode 모두에서 아직 소비하지
않은 수신 packet을 직접 소비하므로 `MANUAL`에서도 `dispatch().submit()`이 필요하지 않다.
`dispatch().submit()`은 등록된 push handler, error·disconnect handler와 request callback을 실행한다.

Java API에서 `submit(...)`은 비동기 작업을 시작한다. **one-way send의 `submit()`은
`CompletionStage<Void>`를 반환한다.** 이 stage는 완료와 실패만 전달하며 전송 결과나 admission
status를 포함하지 않는다. request·wait·lifecycle의 `submit()`은 각 작업의 결과를 담은
`CompletionStage`를 반환한다
([04 §1](../../../server/01-execution/README.ko.md)).
Java connector는 같은 작업을 현재 thread에서 기다리는 별도 blocking terminator를 제공하지 않는다.
lifecycle도 `connect().submit()`, `dispatch().submit()`처럼 같은 call builder 규칙을 따른다.
Kotlin wrapper는 `submit()`으로 얻은
`CompletionStage`를 coroutine suspension으로 기다린다. 이 실행 의미는
[framework 공통 정책](../../../server/01-execution/README.ko.md)을 따른다.

## 4. Options

**기본값은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)이 소유한다.** Java는 이를 flat field를
갖는 record로 표현한다(heartbeat·reconnect를 nested 객체로 두지 않는다).
`createDefault(URI endpoint)`로 기본값 인스턴스를 만든다.

```java
// transport(TCP/TLS/WS/WSS)는 endpoint URI scheme으로 정해진다. heartbeat/reconnect 설정은
// 별도 nested 객체가 아니라 flat field다. `createDefault(URI endpoint)`로 기본값 인스턴스를 만든다.
public record ZLinkStreamConnectorOptions(
    URI endpoint,
    ZLinkStreamDispatchMode dispatchMode,      // default MANUAL
    Duration requestTimeout,                   // default 30s
    Duration waitTimeout,                      // default 5s
    int maxReconnectAttempts,                  // default 3. UNLIMITED_RECONNECT_ATTEMPTS(-1)이 무제한이다
    Duration connectTimeout,                   // default 5s
    int maxSendPayloadSize,                    // default 64 * 1024
    int maxReceivePayloadSize,                 // default 64 * 1024
    boolean heartbeatEnabled,                  // default true
    Duration heartbeatInterval,                // default 1s
    Duration heartbeatTimeout,                 // default 5s
    boolean reconnectEnabled,                  // default true
    Duration reconnectInitialDelay,            // default 250ms
    Duration reconnectMaxDelay,                // default 5s
    double reconnectBackoffFactor,             // default 2.0
    boolean skipServerCertificateValidation,
    ZLinkStreamCompression compression,
    ZLinkStreamCompressionCodec compressionCodec,
    ZLinkStreamPacketNameResolver nameResolver, // 공통 스펙 §5.4의 name resolver 주입점
    ZLinkStreamTypedCodec typedCodec) { // 공통 스펙 §5.4의 typed payload codec 주입점

    // 공통 스펙 §6이 요구하는 무제한 reconnect를 표현하는 이름 붙인 상수다.
    public static final int UNLIMITED_RECONNECT_ATTEMPTS = -1;
}
```

`ZLinkStreamConnectorFactory.create(options)`는 [공통 스펙 §6.3](../../32-stream-connector.ko.md#63-옵션-검증)의
option 검증에 실패하면 `ZLinkStreamException`(§11)을 던진다.

`skipServerCertificateValidation`은 테스트용 자체 서명 인증서에만 사용한다. 운영
기본값은 `false`다. 이 값을 `true`로 바꾸면 TLS transport와 WSS transport 모두 서버
인증서를 신뢰하지 않고 통과시키므로, 운영 환경에서는 사용하면 안 된다.

## 5. Transport와 codec

scheme → transport 매핑과 TLS 검증 규칙은 [공통 스펙 §3](../../32-stream-connector.ko.md)이 소유한다.
Java는 **transport를 별도 enum 옵션으로 고르지 않고 endpoint URI scheme으로 추론한다.**

```java
public enum ZLinkStreamTransport { TCP, TLS, WEB_SOCKET, WEB_SOCKET_SECURE }
public enum ZLinkStreamCodec { RAW, JSON, MESSAGE_PACK, PROTOBUF }
public enum ZLinkStreamCompression { NONE, LZ4 }
```

**호스트명 검증은 `HTTPS` endpoint identification 규칙을 사용한다.**

## 6. Packet 모델

```java
public record ZLinkStreamEncodedPayload(
    String packetName,
    Message payload,
    Map<String, String> metadata,
    ZLinkStreamCodec codec) {
}

public record ZLinkStreamMessage<TPayload>(
    String packetName,
    TPayload payload,
    Map<String, String> metadata,
    String actorId) { // 상대 bound Actor. slot 없는 frame은 null(공통 스펙 §5.6)
}
```

[공통 스펙 §5](../../32-stream-connector.ko.md#5-packet-모델)가 요구하는 **타입에 packet 이름을
붙이는 수단은 annotation이다.**

```java
@Target(ElementType.TYPE)
@Retention(RetentionPolicy.RUNTIME)
public @interface ZLinkStreamPacketName {
    String value();   // 타입에 붙이는 packet 이름
}
```

typed object의 packet identity는 payload type의 `@ZLinkStreamPacketName`을 우선하고,
없으면 type의 `SimpleName`을 사용한다. **호출자가 `packetName(...)`으로 명시하면 그 이름이
우선한다**(공통 스펙 32 §5). 이미 encode한 raw payload의 identity는
`ZLinkStreamEncodedPayload.packetName()`에 명시한다.

metadata는 작은 key-value만 담는다. 큰 업무 데이터는 payload로 보낸다.
STREAM wire header는 runtime 내부 타입이다. connector 사용자와 server session은 header
객체를 만들거나 전달하지 않고, [packet name](../../../server/00-foundation/02-glossary.ko.md#packet-name)과 metadata snapshot만 공개 모델에서 다룬다.

## 7. Send와 Request

```java
public interface ZLinkStreamSendCall {
    ZLinkStreamSendCall packetName(String name);   // 호출별 override. 명시하면 이 이름이 우선한다
    ZLinkStreamSendCall metadata(String key, String value);
    ZLinkStreamSendCall metadata(Map<String, String> metadata);
    ZLinkStreamSendCall compress();
    CompletionStage<Void> submit();
}

public interface ZLinkStreamRequestCall {
    ZLinkStreamRequestCall packetName(String name);   // 호출별 override
    ZLinkStreamRequestCall metadata(String key, String value);
    ZLinkStreamRequestCall metadata(Map<String, String> metadata);
    ZLinkStreamRequestCall timeout(Duration timeout);
    ZLinkStreamRequestCall compress();
    CompletionStage<ZLinkStreamEncodedPayload> submit();
    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);
}

public interface ZLinkTypedStreamSendCall {
    ZLinkTypedStreamSendCall packetName(String name);   // 호출별 override
    ZLinkTypedStreamSendCall metadata(String key, String value);
    ZLinkTypedStreamSendCall metadata(Map<String, String> metadata);
    ZLinkTypedStreamSendCall compress();
    CompletionStage<Void> submit();
}

public interface ZLinkTypedStreamRequestCall {
    ZLinkTypedStreamRequestCall packetName(String name);   // 호출별 override
    ZLinkTypedStreamRequestCall metadata(String key, String value);
    ZLinkTypedStreamRequestCall metadata(Map<String, String> metadata);
    ZLinkTypedStreamRequestCall timeout(Duration timeout);
    ZLinkTypedStreamRequestCall compress();
    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);
}

public interface ZLinkStreamWaitCall {
    ZLinkStreamWaitCall timeout(Duration timeout);
    ZLinkStreamWaitCall where(
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate);
    <TPayload> ZLinkStreamWaitCall where(
        Class<TPayload> payloadType,
        Predicate<ZLinkStreamMessage<TPayload>> predicate);
    CompletionStage<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> submit();
    <TPayload> CompletionStage<ZLinkStreamMessage<TPayload>> submit(
        Class<TPayload> payloadType);
}
```

request timeout의 pending 정리는 [공통 스펙 §5.2](../../32-stream-connector.ko.md#52-request-correlation)가 정한다.
Java는 결과를 `CompletionStage`로 전달한다.


### 7.1 Request hook

[공통 스펙 §5.7](../../32-stream-connector.ko.md#57-요청-hook)의 두 hook은 connector에서
`AutoCloseable onRequestSending(ZLinkStreamRequestSendingHandler)`와
`AutoCloseable onReplyReceived(ZLinkStreamReplyReceivedHandler)`로 등록하고 반환값의 `close()`로 해제한다.

```java
public interface ZLinkStreamRequestSendingHandler {
    void handle(ZLinkStreamRequestSendingContext context);
}
public interface ZLinkStreamReplyReceivedHandler {
    void handle(ZLinkStreamReplyReceivedContext context);
}
```

`ZLinkStreamRequestSendingContext`는 `requestPacketName()`, nullable
`actorId()`, `setMetadata(String key, String value)`를 제공한다. Reply context는
`requestPacketName()`, nullable `actorId()`, `succeeded()`, 성공 시
`reply()`, 실패 시 `error()`, `elapsed()`를 읽기 전용으로 제공한다.
metadata 검증과 hook 실패의 처리 원칙은 공통 스펙 §5.7과 §7을 따른다.


### 7.2 테스트 대기 표면

계약은 [공통 스펙 §10.1](../../32-stream-connector.ko.md)가 소유한다. Java 표면은 다음과 같다.

**push 관측 — connector 메서드**(`waitFor`와 같은 자리). 각각 builder를 반환한다.

각 표면은 **호출자가 packet 이름을 명시하는 overload와 payload type에서 결정하는 overload를
함께** 둔다([공통 스펙 §10.1.1](../../32-stream-connector.ko.md#1011-push-관측-표면--waitfor-계열)).
type overload는 options의 `nameResolver`가 이름을 결정한다.

```java
ZLinkStreamWaitCall       waitFor(String name);              // 도달할 때까지 대기
ZLinkStreamWaitCall       waitFor(Class<?> payloadType);
ZLinkStreamExpectNoneCall expectNone(String name);           // .within(window) 동안 오지 않는지
ZLinkStreamExpectNoneCall expectNone(Class<?> payloadType);
ZLinkStreamSequenceCall   waitForSequence(String name);      // .expect(p).expect(p)…를 순서대로
ZLinkStreamSequenceCall   waitForSequence(Class<?> payloadType);

public interface ZLinkStreamExpectNoneCall {
    ZLinkStreamExpectNoneCall within(Duration window);       // 관찰 구간. 지정해야 한다
    CompletionStage<Void> submit();
}

public interface ZLinkStreamSequenceCall {
    // 도착 순서대로 적용할 다음 술어를 더한다. 인자는 payload가 아니라 message다.
    ZLinkStreamSequenceCall expect(
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate);
    <TPayload> ZLinkStreamSequenceCall expect(
        Class<TPayload> payloadType,
        Predicate<ZLinkStreamMessage<TPayload>> predicate);
    ZLinkStreamSequenceCall timeout(Duration timeout);
    CompletionStage<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> submit();
    <TPayload> CompletionStage<List<ZLinkStreamMessage<TPayload>>> submit(
        Class<TPayload> payloadType);
}
```

- `expectNone(name).within(Duration).submit()` — window 안에 도착하면 **`VALIDATION_FAILED`를 담은 `ZLinkStreamException`으로 실패한다**. `waitFor`의 대칭.
- `waitForSequence(name).expect(p1).expect(p2)….timeout(t).submit()` — `List<ZLinkStreamMessage<TPayload>>`를 반환하며, 순서 관측과 실패는 [공통 스펙 §10.1](../../32-stream-connector.ko.md#101-테스트-대기-표면)이 정한다.
- **술어와 반환은 `ZLinkStreamMessage`를 다룬다.** `where(...)`와 `expect(...)`가 받는 인자도 payload가 아니라 message다.
- **status 전용 표면을 두지 않는다.** status는 payload 필드이므로 `waitFor(T.class).where(T.class, m -> m.payload().status() == …)`로 표현한다.

- **도메인 REST 폴링은 이 표면이 아니다.** 그건 `ZLinkHttpClient`의 일이다.

## 8. Typed payload codec

기본 connector는 wire payload를 `ZLinkStreamEncodedPayload`로 보관한다. typed 표면은
options의 **`typedCodec` 하나**를 사용해 업무 DTO를 encode/decode한다(기본은 JSON).
application code는 일반적으로 raw `Message`나 codec helper를 직접 다루지 않는다.

위의 `ZLinkStreamConnector.send(Object)`, `request(Object)`,
`on(Class<TPayload>, ...)`, `waitFor(...)`가 typed payload 표면이다.

typed 표면이 만드는 packet name은 core connector의 name resolver를 그대로 사용한다.
codec으로 표현할 수 없는 payload는 configuration error로 실패한다.
server push를 기다릴 때는 기본 connector의 wait builder를
사용한다. payload 조건이 필요하면
`connector.waitFor(name).where(payloadType, predicate).submit(payloadType)`처럼
core wait builder의 `where`를 사용한다. sample client는 server push를 기다릴 때
connector member `waitFor(...).where(...).submit(...)` 또는 Kotlin wrapper
`waitFor<T>(...).where { ... }.await()` 형태를 사용한다.
typed 표면은 registry가 encode/decode할 수 있는 업무 객체 payload를 기준으로
동작한다. `String`, `byte[]`, `Message` 같은 raw payload는 connector 하위 경로나
명시적 raw 사용에서만 다룬다.

Kotlin 표면은 Java call을 application에 직접 노출하지 않고 전용 wrapper로 감싼다. Reply type은 request
wrapper를 만들 때 고정하므로 terminal에서 type이나 operation 이름을 반복하지 않는다.

```kotlin
// Reply type은 request를 만들 때 wrapper에 고정하고 await()로 결과를 기다린다.
val reply: LoginReply = connector
    .request<LoginReply>(LoginRequest("user-1"))
    .await()

// Server push도 Kotlin 전용 wait wrapper의 await()로 기다린다.
val pushed: ZLinkStreamMessage<Notice> = connector
    .waitFor<Notice>()
    .where { it.payload.important }
    .await()
```

## 9. Dispatch Mode

```java
public enum ZLinkStreamDispatchMode {
    MANUAL,     // 기본값
    IMMEDIATE   // receive 경로에서 인라인 실행한다(공통 스펙 32 §7)
}
```

`MANUAL`과 `IMMEDIATE`의 callback 실행과 `dispatch().submit()`의 관계는
[공통 스펙 §7](../../32-stream-connector.ko.md#7-dispatch-모드)이 정한다.

## 10. 연결 상태

상태의 의미와 전이는 [공통 스펙 §6](../../32-stream-connector.ko.md)이 소유한다. Java는 닫힌 enum으로
표현한다.

```java
public enum ZLinkStreamConnectionState {
    CREATED,
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECONNECTING,
    CLOSED
}
```

`close().submit()`이 완료된 뒤에는 `CLOSED`이고 **새 `connect()`는 실패한다.**

**`CREATED`는 첫 연결 시도 전의 초기 상태다.** 연결 시도에 실패한 뒤에는 `DISCONNECTED`로
전환하므로, "한 번도 연결한 적 없음"과 "끊김"을 구분한다.

## 11. Error Code

오류의 의미는 [공통 스펙 §9](../../32-stream-connector.ko.md)가 소유한다. Java는 닫힌 enum으로
표현한다. [공통 스펙 §9.2](../../32-stream-connector.ko.md#92-전달--받는-쪽이-코드를-읽을-수-있어야-한다)가
요구하는 **코드를 담는 전용 예외 타입은 `ZLinkStreamException`이다.** caller가 취소한 operation의
`CompletableFuture`는 `CancellationException`으로 끝나며 `ZLinkStreamException`이 아니다
([공통 스펙 §5.2](../../32-stream-connector.ko.md#52-request-correlation)).

```java
public record ZLinkStreamError(
    ZLinkStreamErrorCode code,   // 아래 닫힌 13개 값
    String message,
    Throwable exception) {       // 원인 예외. 없으면 null
}

public final class ZLinkStreamException extends RuntimeException {
    public ZLinkStreamException(ZLinkStreamError error);
    public ZLinkStreamError error();           // 코드를 읽는 자리다
    public ZLinkStreamErrorCode errorCode();   // error().code()의 단축이다
}
```

connector가 던지거나 `CompletionStage`를 실패로 완료할 때 쓰는 예외는 `ZLinkStreamException`
하나다. **`IllegalArgumentException`·`IllegalStateException` 같은 언어 표준 예외를 그대로 던지지
않는다** — 그 타입에는 코드를 담을 자리가 없어 호출자가 `VALIDATION_FAILED`인지
`CONFIGURATION_ERROR`인지 판정하지 못한다. 옵션 검증 실패(§4)와 대기 표면의 위반(§7.2)도 같은
예외로 전달한다.

```java
public enum ZLinkStreamErrorCode {
    DISCONNECTED,
    CONFIGURATION_ERROR,
    VALIDATION_FAILED,
    REQUEST_TIMEOUT,
    CONNECT_TIMEOUT,
    FRAME_DECODE_FAILED,
    FRAME_TOO_LARGE,
    SEND_FAILED,
    COMPRESSION_FAILED,
    TLS_VALIDATION_FAILED,
    DECOMPRESSION_FAILED,
    USER_CALLBACK_FAILED,
    REMOTE_ERROR
}
```

## 12. Kotlin 표면

Kotlin module은 Java connector 위의 thin wrapper다. lifecycle과 request처럼 완료값이
있는 작업은 Kotlin wrapper의 suspend `await()`로 기다린다. 이 `await()`는 Java
`CompletionStage`를 coroutine suspension으로 기다린다. one-way send도 `await()`로 완료와
실패를 기다리지만 전송 결과나 admission status는 받지 않는다.

**Kotlin은 별도 예외 계층을 두지 않고 Java의 `ZLinkStreamException`(§11)을 그대로 전파한다.**
`await()`가 실패하면 같은 예외가 호출 지점에서 발생하며, 호출자는 `error().code()`로 오류
코드를 읽는다.

```kotlin
fun ZLinkStreamConnector.kotlin(): ZLinkKotlinStreamConnector

fun ZLinkStreamConnectorOptions.withDefaultStreamCompression(): ZLinkStreamConnectorOptions
fun ZLinkStreamConnectorOptions.withLz4StreamCompression(): ZLinkStreamConnectorOptions
fun ZLinkStreamConnectorOptions.withStreamCompression(
    codec: ZLinkStreamCompressionCodec,
): ZLinkStreamConnectorOptions
fun ZLinkStreamConnectorOptions.withoutStreamCompression(): ZLinkStreamConnectorOptions

class ZLinkKotlinStreamConnector {
    fun on(name: String, handler: ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>): AutoCloseable
    inline fun <reified TPayload> on(handler: ZLinkStreamMessageHandler<TPayload>): AutoCloseable
    fun <TPayload : Any> on(name: String, payloadType: KClass<TPayload>, handler: ZLinkStreamMessageHandler<TPayload>): AutoCloseable
    fun receivedCount(name: String): Int
    fun connect(): ZLinkKotlinLifecycleCall
    fun close(): ZLinkKotlinLifecycleCall
    fun dispatch(): ZLinkKotlinLifecycleCall
    fun send(payload: ZLinkStreamEncodedPayload): ZLinkKotlinSendCall
    fun send(payload: Any): ZLinkKotlinSendCall
    fun request(
        payload: ZLinkStreamEncodedPayload,
    ): ZLinkKotlinRawRequestCall
    fun <TReply : Any> request(
        payload: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
    fun closeReason(): ZLinkStreamCloseReason?
    fun <TPayload> waitFor(): ZLinkStreamTypedWaitCall<TPayload>
    fun <TPayload> waitFor(name: String): ZLinkStreamTypedWaitCall<TPayload>
    fun <TPayload> expectNone(): ZLinkStreamTypedExpectNoneCall<TPayload>
    fun <TPayload> expectNone(name: String): ZLinkStreamTypedExpectNoneCall<TPayload>
    fun <TPayload> waitForSequence(): ZLinkStreamTypedSequenceCall<TPayload>
    fun <TPayload> waitForSequence(name: String): ZLinkStreamTypedSequenceCall<TPayload>
    fun messages(packetName: String): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>
    inline fun <reified TPayload> messages(): Flow<ZLinkStreamMessage<TPayload>>
    fun <TPayload : Any> messages(packetName: String, payloadType: KClass<TPayload>): Flow<ZLinkStreamMessage<TPayload>>
    fun onRequestSending(handler: ZLinkStreamRequestSendingHandler): AutoCloseable
    fun repliesReceived(): Flow<ZLinkStreamReplyReceivedContext>
    fun errors(): Flow<ZLinkStreamError>
    fun actors(): List<ZLinkKotlinStreamActor>
    fun actor(actorId: String): ZLinkKotlinStreamActor?
    fun actorBound(): Flow<ZLinkKotlinStreamActor>
    fun actorUnbound(): Flow<ZLinkKotlinStreamActor>
}

class ZLinkKotlinStreamActor {
    fun on(name: String, handler: ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>): AutoCloseable
    inline fun <reified TPayload> on(handler: ZLinkStreamMessageHandler<TPayload>): AutoCloseable
    fun <TPayload : Any> on(name: String, payloadType: KClass<TPayload>, handler: ZLinkStreamMessageHandler<TPayload>): AutoCloseable
    val actorId: String
    val isBound: Boolean
    fun send(payload: ZLinkStreamEncodedPayload): ZLinkKotlinSendCall
    fun send(payload: Any): ZLinkKotlinSendCall
    fun request(payload: ZLinkStreamEncodedPayload): ZLinkKotlinRawRequestCall
    fun <TReply : Any> request(
        payload: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
    fun messages(packetName: String): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>
    inline fun <reified TPayload> messages(): Flow<ZLinkStreamMessage<TPayload>>
    fun <TPayload : Any> messages(packetName: String, payloadType: KClass<TPayload>): Flow<ZLinkStreamMessage<TPayload>>
}

inline fun <reified TReply : Any> ZLinkKotlinStreamActor.request(
    payload: Any,
): ZLinkKotlinRequestCall<TReply> =
    request(payload, TReply::class)

class ZLinkKotlinLifecycleCall {
    suspend fun await()
}

class ZLinkKotlinSendCall {
    fun packetName(name: String): ZLinkKotlinSendCall
    suspend fun await(): Unit
}

class ZLinkKotlinRawRequestCall {
    fun packetName(name: String): ZLinkKotlinRawRequestCall
    fun metadata(key: String, value: String): ZLinkKotlinRawRequestCall
    fun timeout(timeout: Duration): ZLinkKotlinRawRequestCall
    fun compress(): ZLinkKotlinRawRequestCall
    suspend fun await(): ZLinkStreamEncodedPayload
}

class ZLinkKotlinRequestCall<TReply : Any> {
    fun packetName(name: String): ZLinkKotlinRequestCall<TReply>
    fun metadata(key: String, value: String): ZLinkKotlinRequestCall<TReply>
    fun timeout(timeout: Duration): ZLinkKotlinRequestCall<TReply>
    fun compress(): ZLinkKotlinRequestCall<TReply>
    suspend fun await(): TReply
}

inline fun <reified TReply : Any> ZLinkKotlinStreamConnector.request(
    payload: Any,
): ZLinkKotlinRequestCall<TReply> =
    request(payload, TReply::class)

class ZLinkStreamTypedWaitCall<TPayload> {
    fun timeout(timeout: Duration): ZLinkStreamTypedWaitCall<TPayload>
    fun where(predicate: (ZLinkStreamMessage<TPayload>) -> Boolean): ZLinkStreamTypedWaitCall<TPayload>
    suspend fun await(): ZLinkStreamMessage<TPayload>
}

class ZLinkStreamTypedExpectNoneCall<TPayload> {
    fun within(window: Duration): ZLinkStreamTypedExpectNoneCall<TPayload>
    suspend fun await()   // window 안에 도착하면 예외
}

class ZLinkStreamTypedSequenceCall<TPayload> {
    fun expect(predicate: (ZLinkStreamMessage<TPayload>) -> Boolean): ZLinkStreamTypedSequenceCall<TPayload>
    fun timeout(timeout: Duration): ZLinkStreamTypedSequenceCall<TPayload>
    suspend fun await(): List<ZLinkStreamMessage<TPayload>>   // 술어 순서대로 도착
}

```

**종료 사유와 Actor handle은 wrapper에서 직접 얻는다.** wrapper만 사용하는 코드도 여기에 닿아야
하므로 Java connector를 꺼내 쓰도록 두지 않는다. Java의 `Optional`은 Kotlin의 nullable로 옮긴다 —
종료 사유는 한 번도 끊긴 적이 없으면 `null`이고, `actor(id)`는 bind되지 않은 id에 `null`이다
([공통 스펙 §5.6](../../32-stream-connector.ko.md#56-bound-actor)).

**대기 표면 셋은 이름을 명시하는 길과 payload type에서 결정하는 길을 모두 제공한다.** 이름을
주지 않으면 `TPayload`의 이름 결정 규칙(§5)이 정한다. `waitFor`만 두 길을 갖고 `expectNone`과
`waitForSequence`가 이름을 요구하면, 같은 시험 안에서 세 표면의 호출 형태가 갈린다.

Kotlin wrapper는 Java connector와 다른 상태 전이나 buffering 정책을 만들면 안 된다. options를
복사하는 extension은 **현재 정의된 모든 option 값을 보존해야 한다.**
dispatch mode를 따르는 callback의 `Flow` 표면은 대응하는 Java 등록을 `callbackFlow`로 감싼다 — connector의 `messages(...)`·
`errors()`·`actorBound()`·`actorUnbound()`는 `on(...)`·`onErrorReceived(...)`·`onActorBound(...)`·
`onActorUnbound(...)`를, Actor의 `messages(...)`는 그 Actor handle의 `ZLinkStreamActor.on(...)`을 감싼다. 따라서 manual [dispatch mode](../../../server/00-foundation/02-glossary.ko.md#dispatch-mode)에서는 Java와 마찬가지로
Kotlin wrapper의 `dispatch().await()`가 호출되어야 collector가 메시지나 error event를 받는다.

reply received hook은 §7 dispatch mode를 따르므로 Kotlin의
`repliesReceived(): Flow`로 투영한다. request sending hook은 §5.7에 따라 요청을
호출한 스레드에서 frame 생성 전에 동기로 실행하므로
`onRequestSending(handler: ZLinkStreamRequestSendingHandler): AutoCloseable`로
등록한다. 이 handler는 metadata를 동기로 변경하고 반환값이나 cancellation 인자가 없다.

## 13. 검증 기준

Java connector는 아래 테스트를 별도 suite로 가진다.

- public API export test
- transport scheme inference와 mismatch validation
- header encode/decode roundtrip
- metadata validation
- send frame size limit
- request timeout pending cleanup
- manual dispatch queue와 `pendingDispatchCount`
- immediate dispatch callback
- heartbeat ping/pong과 timeout
- reconnect backoff와 max attempts
- typed handler registry add/remove
- JSON, MessagePack, Protobuf codec smoke
- typed helper packet name resolver와 codec selection
- typed request/reply decode
- request hook 등록 순서·metadata·실패 결과·callback 오류
- flow 필드 구조 검사 후 값 폐기와 outbound flag 0x10 미설정
- connector와 Actor의 packet 이름 두 형태
- Kotlin coroutine/Flow wrapper smoke

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../../../../README.ko.md) | [이전: Java STREAM](../../../server/languages/java/interfaces/stream-session.ko.md)
<!-- framework-adapter-nav:bottom:end -->
