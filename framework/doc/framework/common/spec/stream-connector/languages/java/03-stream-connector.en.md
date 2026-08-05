<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md) | [Previous: Java STREAM](../../../server/languages/java/interfaces/stream-session.en.md)
<!-- framework-adapter-nav:end -->

[Java spec table of contents](../../../server/languages/java/README.en.md)

[Java Bundle](../../../../../java/README.en.md) | [STREAM](../../../server/languages/java/interfaces/stream-session.en.md) | [Samples](../../../../../../../languages/java/samples/README.md)

# Java/Kotlin Stream Connector

> This document is the **Java/Kotlin projection** of the
> [Stream Connector Common Spec](../../32-stream-connector.en.md).
> Transport/wire/lifecycle/error meaning is owned by the common spec,
> and this document fixes the **exact public surface** that meaning
> has in Java/Kotlin.

## 1. Goal

Stream Connector is a module separate from the server framework. It
lets an external client build and interpret the same
framework-header-based STREAM packet the server's `ZLinkSession`
receives.

This module doesn't depend on a Spring Boot server adapter, SPOT, or
Registry. It only has the dependency needed for client execution, such
as transport, codec, compression, reconnect, and dispatch queue.

### 1.1 Target Execution Environment

**The connector responsible per engine × build target is owned by
[Common Spec §2](../../32-stream-connector.en.md).** Per that
assignment, what the Java/Kotlin connector is responsible for is a
**JVM application** (server tool/E2E test/bot), and it isn't
responsible for a game engine or browser.

Since there's only one target, this assignment leaves no effect on the
Java/Kotlin surface. The usage guide only separately describes the
per-language async usage in the
[Java guide](../../../../../java/guide/stream-connector/README.en.md)
and the
[Kotlin guide](../../../../../kotlin/guide/stream-connector/README.en.md).

## 2. Module

The Java connector's Maven coordinate is
`systems.zlink:zlink-stream-connector`.

| Module | Role |
|------|------|
| `zlink-stream-connector` | TCP/TLS/WS/WSS transport, frame codec, send/request, dispatch |
| `zlink-framework-kotlin` | coroutine, `Flow`, DSL extension |
| `zlink-framework-codec-protobuf` | The Protobuf codec extension shared across framework/connector/http-client |
| `zlink-framework-codec-msgpack` | The MessagePack codec extension shared across framework/connector/http-client |

JSON is the framework default codec. A Protobuf and MessagePack
payload isn't a connector-dedicated package — it's used by applying
the `zlink-framework-codec-protobuf`, `zlink-framework-codec-msgpack`
framework codec extension to the connector too.

## 3. Public API

```java
public interface ZLinkStreamConnector {
    boolean isConnected();
    ZLinkStreamConnectionState state();
    ZLinkStreamConnectorOptions options();
    int pendingDispatchCount();
    int receivedCount(String name);

    // there are only three lifecycle surfaces. Manual reconnect is a state
    // transition of connect(), and automatic reconnect is handled by options
    // (Common Spec 32 §6).
    ZLinkStreamLifecycleCall connect();
    ZLinkStreamLifecycleCall close();
    ZLinkStreamLifecycleCall dispatch();

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);
    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);
    ZLinkTypedStreamSendCall send(Object payload);
    ZLinkTypedStreamRequestCall request(Object payload);
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
    AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler);
    AutoCloseable onDisconnected(ZLinkStreamDisconnectedHandler handler);
    AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler);
    AutoCloseable observeInbound(ZLinkStreamInboundObserver observer);
}

public interface ZLinkStreamLifecycleCall {
    CompletionStage<Void> submit();
}

public final class ZLinkStreamConnectorFactory {
    public static ZLinkStreamConnector create(ZLinkStreamConnectorOptions options);
}
```

Java exposes an event as an `on...` registration. The meaning is the
same as .NET's event. Deregistration is done through the returned
`AutoCloseable`.

**Session close reason.** The value set and meaning is owned by
[Common Spec §6.3](../../32-stream-connector.en.md#63-close-reason).
Java expresses this as the closed enum `ZLinkStreamCloseReason`
(`CLIENT_CLOSE`, `IDLE_TIMEOUT`, `HEARTBEAT_TIMEOUT`, `SERVER_DRAIN`,
`PROTOCOL_ERROR`, `TRANSPORT_ERROR`), and **exposes it as the disconnect
event's `ZLinkStreamCloseReason closeReason()`, which
`ZLinkStreamDisconnectedHandler` receives.**
`waitFor(...)` returns a call builder that waits once for a server push
of a specific packet name. When only a specific message is needed, use
the builder's `where(...)`. Once the timeout passes, the returned
`CompletionStage` ends with a timeout failure. If a separate timeout
isn't specified, the connector options' `waitTimeout()` value is used.
Since `waitFor(...)` directly consumes an unconsumed receive packet in
both dispatch modes, `dispatch().submit()` isn't needed in `MANUAL`
either. `dispatch().submit()` runs a registered push handler, error/
disconnect handler, and request callback.

In the Java API, `submit(...)` starts an async operation. **A one-way
send's `submit()` returns `CompletionStage<Void>`.** This stage only
delivers completion and failure, and doesn't include transport result
or admission status. A request/wait/lifecycle's `submit()` returns a
`CompletionStage` carrying each operation's result
([04 §1](../../../05-async-execution-policy.en.md)).
The Java connector doesn't provide a separate blocking terminator that
waits for the same operation on the current thread. Lifecycle also
follows the same call builder convention, like `connect().submit()`,
`dispatch().submit()`.
The Kotlin wrapper waits for the `CompletionStage` obtained from
`submit()` as a coroutine suspension. This execution meaning follows
the [framework common policy](../../../05-async-execution-policy.en.md).

## 4. Options

**The default value is owned by
[Common Spec §6.1](../../32-stream-connector.en.md).** Java expresses
this as a record with a flat field (heartbeat/reconnect aren't put as
a nested object). A default instance is built with
`createDefault(URI endpoint)`.

```java
// transport (TCP/TLS/WS/WSS) is decided by the endpoint URI scheme. Heartbeat/reconnect
// configuration is a flat field, not a separate nested object. A default instance is built
// with `createDefault(URI endpoint)`.
public record ZLinkStreamConnectorOptions(
    URI endpoint,
    ZLinkStreamDispatchMode dispatchMode,      // default MANUAL
    Duration requestTimeout,                   // default 30s
    Duration waitTimeout,                      // default 5s
    int maxReconnectAttempts,                  // default 3
    Duration connectTimeout,                   // default 5s
    int maxSendPayloadSize,                    // default 64 * 1024
    int maxReceivePayloadSize,                 // default 64 * 1024
    int maxReceivedMessages,                   // default 1024 (the receive message queue bound)
    int maxInboundObserverNotifications,       // default 1024
    int maxInboundObserverPayloadPreviewBytes, // default 0
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
    ZLinkStreamPacketNameResolver nameResolver,
    ZLinkStreamTypedCodec typedCodec) {
}
```

`skipServerCertificateValidation` is used only for a test's self-signed
certificate. The production default is `false`. Setting this value to
`true` passes both TLS transport and WSS transport without trusting the
server certificate, so it must not be used in a production
environment.

## 5. Transport And Codec

The scheme → transport mapping and TLS validation rule is owned by
[Common Spec §3](../../32-stream-connector.en.md). Java **infers the
transport from the endpoint URI scheme instead of choosing it as a
separate enum option.**

```java
public enum ZLinkStreamTransport { TCP, TLS, WEB_SOCKET, WEB_SOCKET_SECURE }
public enum ZLinkStreamCodec { RAW, JSON, MESSAGE_PACK, PROTOBUF }
public enum ZLinkStreamCompression { NONE, LZ4 }
```

**Hostname validation uses the `HTTPS` endpoint identification rule.**

## 6. Packet Model

```java
public record ZLinkStreamEncodedPayload(
    String packetName,
    Message payload,
    Map<String, String> metadata,
    ZLinkStreamCodec codec) {
}

public enum ZLinkFlowOrigin {
    INBOUND,
    TIMER,
    APPLICATION,
    LIFECYCLE
}

public interface ZLinkStreamFlow {
    String flowId();
    ZLinkFlowOrigin flowOrigin();
}

public record ZLinkStreamMessage<TPayload>(
    String packetName,
    TPayload payload,
    Map<String, String> metadata,
    String flowId,
    ZLinkFlowOrigin flowOrigin) implements ZLinkStreamFlow {
}
```

A typed object's packet identity prioritizes the payload type's
`@ZLinkStreamPacketName`, and uses the type's `SimpleName` if absent.
**If the caller specifies it with `packetName(...)`, that name takes
priority** (Common Spec 32 §5). The identity of an already-encoded raw
payload is specified in `ZLinkStreamEncodedPayload.packetName()`.

Metadata only carries a small key-value. Large work data is sent as
payload. The STREAM wire header is a runtime-internal type. The
connector user and server session don't build or pass a header object
— they only handle the
[packet name](../../../01-glossary.en.md#packet-name) and a metadata
snapshot in the public model.

## 7. Send And Request

```java
public interface ZLinkStreamSendCall {
    ZLinkStreamSendCall packetName(String name);   // per-call override. If specified, this name takes priority
    ZLinkStreamSendCall metadata(String key, String value);
    ZLinkStreamSendCall metadata(Map<String, String> metadata);
    ZLinkStreamSendCall compress();
    CompletionStage<Void> submit();
}

public interface ZLinkStreamRequestCall {
    ZLinkStreamRequestCall packetName(String name);   // per-call override
    ZLinkStreamRequestCall metadata(String key, String value);
    ZLinkStreamRequestCall metadata(Map<String, String> metadata);
    ZLinkStreamRequestCall timeout(Duration timeout);
    ZLinkStreamRequestCall compress();
    CompletionStage<ZLinkStreamEncodedPayload> submit();
    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);
}

public interface ZLinkTypedStreamSendCall {
    ZLinkTypedStreamSendCall packetName(String name);   // per-call override
    ZLinkTypedStreamSendCall metadata(String key, String value);
    ZLinkTypedStreamSendCall metadata(Map<String, String> metadata);
    ZLinkTypedStreamSendCall compress();
    CompletionStage<Void> submit();
}

public interface ZLinkTypedStreamRequestCall {
    ZLinkTypedStreamRequestCall packetName(String name);   // per-call override
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

Once the request timeout ends, the pending request is removed and the
returned `CompletionStage` completes with a timeout failure. Even if
the removed request's response arrives late, that stage isn't
completed again.

### 7.1 Flow Correlation

An outbound operation the Connector starts generates a UUIDv7
`flow_id` once. The connector runtime sets the current flow context for
the inbound handler's execution scope. A related outbound started from
that handler reuses the same `flowId` and `flowOrigin` with no separate
public argument, and restores the previous context at the handler's
terminal completion. The flow isn't propagated to an unrelated next
callback or a separate executor the Framework doesn't manage, and an
outbound started there starts a new flow with `APPLICATION` origin.

The current flow isn't guessed from a connector instance's mutable
field or thread ID. The wire format and async context boundary is
owned by
[Stream Connector §4.2](../../32-stream-connector.en.md#42-header) and
[Flow Correlation §6](../../../27-flow-correlation.en.md#6-async-work-and-execution-context).

### 7.2 Test Wait Surface

The contract is owned by
[Common Spec §10.2](../../32-stream-connector.en.md). The Java surface
is below.

**Push observation — connector method** (the same spot as `waitFor`).
Each returns a builder.

```java
ZLinkStreamWaitCall       waitFor(String name);          // waits until it arrives
ZLinkStreamExpectNoneCall expectNone(String name);       // whether it doesn't arrive during .within(window)
ZLinkStreamSequenceCall   waitForSequence(String name);  // .expect(p).expect(p)… in order
```

- `expectNone(name).within(Duration).submit()` — **throws an exception**
  if it arrives within the window. The symmetric of `waitFor`.
- `waitForSequence(name).expect(p1).expect(p2)….timeout(t).submit()` —
  confirms a push of the same name arrives **in predicate order**, and
  returns the payload list. Verifies **"arrived in order"**, not "N
  arrived."
- **A status-only surface isn't provided.** Since status is a payload
  field, it's expressed as
  `waitFor(T.class).where(p -> p.status() == …)`.

- **Domain REST polling isn't this surface.** That's `ZLinkHttpClient`'s
  job.

## 8. Typed Payload Codec

The base connector keeps the wire payload as `ZLinkStreamEncodedPayload`.
The typed surface uses options' **single `typedCodec`** to encode/
decode the work DTO (default is JSON). Application code generally
doesn't directly handle a raw `Message` or codec helper.

The `ZLinkStreamConnector.send(Object)`, `request(Object)`,
`on(Class<TPayload>, ...)`, `waitFor(...)` above are the typed payload
surface.

The packet name the typed surface builds directly uses the core
connector's name resolver as is. A payload that can't be expressed by
the codec fails as a configuration error.
When waiting for a server push, the base connector's wait builder is
used. If a payload condition is needed, the core wait builder's `where`
is used, like
`connector.waitFor(name).where(payloadType, predicate).submit(payloadType)`.
A sample client waits for a server push using the connector member
`waitFor(...).where(...).submit(...)` or the Kotlin wrapper
`waitFor<T>(...).where { ... }.await()` form.
The typed surface operates based on a work object payload the registry
can encode/decode. A raw payload, such as `String`, `byte[]`, `Message`,
is only handled on a connector sub-path or explicit raw use.

The Kotlin surface doesn't directly expose the Java call to the
application — it wraps it with a dedicated wrapper. Reply type is
fixed when building the request wrapper, so type or operation name
isn't repeated in the terminal.

```kotlin
// Reply type is fixed on the wrapper when building the request, and the result is awaited with await().
val reply: LoginReply = connector
    .request<LoginReply>(LoginRequest("user-1"))
    .await()

// Server push is also awaited with the Kotlin-dedicated wait wrapper's await().
val pushed: ZLinkStreamMessage<Notice> = connector
    .waitFor<Notice>()
    .where { it.payload.important }
    .await()
```

## 9. Dispatch Mode

```java
public enum ZLinkStreamDispatchMode {
    MANUAL,     // default
    IMMEDIATE   // runs inline on the receive path (Common Spec 32 §7)
}
```

The default is `MANUAL`. The receive loop, reconnect loop, and request
callback task don't directly call a user handler — they put it in the
dispatch queue. The application calls `dispatch().submit()` on the
thread of its choice.

`IMMEDIATE` runs the callback inline on the receive path, so a slow
handler blocks the receive loop and backpressure applies accordingly.
A client sample with a UI thread or game loop keeps `MANUAL`.

## 10. Connection State

The state's meaning and transition is owned by
[Common Spec §6](../../32-stream-connector.en.md). Java expresses this
as a closed enum.

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

Once `close().submit()` completes, it's `CLOSED` and **a new
`connect()` fails.**

**`CREATED` is the initial state before the first connection attempt.**
Once a connection attempt fails, it switches to `DISCONNECTED`, so
"never connected" and "disconnected" are distinguished.

## 11. Error Code

The error's meaning is owned by
[Common Spec §9](../../32-stream-connector.en.md). Java expresses this
as a closed enum.

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
    OBSERVER_FAILED,
    OBSERVER_DROPPED,
    RECEIVED_MESSAGE_DROPPED,   // receive message queue overflow (Common Spec 32 §10)
    REMOTE_ERROR
}
```

## 12. Inbound Observer

Observation meaning and the isolation/overflow rule is owned by
[Common Spec §10](../../32-stream-connector.en.md). Java **registers
only before connection starts** and deregisters with `AutoCloseable`.

```java
try (AutoCloseable log = connector.observeInbound(observation -> {
    System.out.printf(
        "stream-inbound kind=%s name=%s seq=%s bytes=%d%n",
        observation.kind(),
        observation.packetName(),
        observation.requestSeq(),
        observation.payloadLength());
})) {
    connector.connect().submit(); // connection completion is observed through the returned CompletionStage.
}
```

### 12.1 Metric

The Java connector publishes
[Common Spec §6.2](../../32-stream-connector.en.md#62-connector-reconnect-instrument)'s
`zlink.stream.reconnects` and its closed tag to the Micrometer global
registry. The application and E2E register a public `MeterRegistry`
with `Metrics.addRegistry(...)` and read the counter from the same
registry. The Kotlin wrapper also uses the same registry as the Java
connector. A registry or listener failure doesn't change send, request,
or connection state.

## 13. Kotlin Surface

The Kotlin module is a thin wrapper on top of the Java connector. An
operation with a completion value, such as lifecycle and request, is
awaited with the Kotlin wrapper's suspend `await()`. This `await()`
waits for the Java `CompletionStage` as a coroutine suspension. A
one-way send also waits for completion and failure with `await()`, but
doesn't receive a transport result or admission status.

```kotlin
fun ZLinkStreamConnector.kotlin(): ZLinkKotlinStreamConnector

fun ZLinkStreamConnectorOptions.withDefaultStreamCompression(): ZLinkStreamConnectorOptions
fun ZLinkStreamConnectorOptions.withLz4StreamCompression(): ZLinkStreamConnectorOptions
fun ZLinkStreamConnectorOptions.withStreamCompression(
    codec: ZLinkStreamCompressionCodec,
): ZLinkStreamConnectorOptions
fun ZLinkStreamConnectorOptions.withoutStreamCompression(): ZLinkStreamConnectorOptions

class ZLinkKotlinStreamConnector {
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
    fun <TPayload> waitFor(): ZLinkStreamTypedWaitCall<TPayload>
    fun <TPayload> waitFor(name: String): ZLinkStreamTypedWaitCall<TPayload>
    fun <TPayload> expectNone(name: String): ZLinkStreamTypedExpectNoneCall<TPayload>
    fun <TPayload> waitForSequence(name: String): ZLinkStreamTypedSequenceCall<TPayload>
    fun messages(packetName: String): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>
    fun errors(): Flow<ZLinkStreamError>
}

class ZLinkKotlinLifecycleCall {
    suspend fun await()
}

class ZLinkKotlinSendCall {
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
    suspend fun await()   // exception if it arrives within the window
}

class ZLinkStreamTypedSequenceCall<TPayload> {
    fun expect(predicate: (ZLinkStreamMessage<TPayload>) -> Boolean): ZLinkStreamTypedSequenceCall<TPayload>
    fun timeout(timeout: Duration): ZLinkStreamTypedSequenceCall<TPayload>
    suspend fun await(): List<ZLinkStreamMessage<TPayload>>   // arrives in predicate order
}

```

The Kotlin wrapper must not build a different state transition or
buffering policy from the Java connector. The extension copying options
**must preserve every option value, including the receive message
bound.** `messages(...)` and `errors()` wrap the Java connector's
`on(...)`, `onErrorReceived(...)` handler with `callbackFlow`. So in
manual [dispatch mode](../../../01-glossary.en.md#dispatch-mode), just
like Java, the Kotlin wrapper's `dispatch().await()` must be called for
the collector to receive a message or error event.

## 14. Verification Standard

The Java connector has the tests below as a separate suite.

- Public API export test
- Transport scheme inference and mismatch validation
- Header encode/decode roundtrip
- Metadata validation
- Send frame size limit
- Request timeout pending cleanup
- Manual dispatch queue and `pendingDispatchCount`
- Immediate dispatch callback
- Heartbeat ping/pong and timeout
- Reconnect backoff and max attempts
- Typed handler registry add/remove
- JSON, MessagePack, Protobuf codec smoke
- Typed helper packet name resolver and codec selection
- Typed request/reply decode
- Inbound observer response/send/control observation, callback failure,
  queue overflow
- Kotlin coroutine/Flow wrapper smoke

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](../../../../../../README.en.md) | [Previous: Java STREAM](../../../server/languages/java/interfaces/stream-session.en.md)
<!-- framework-adapter-nav:bottom:end -->
