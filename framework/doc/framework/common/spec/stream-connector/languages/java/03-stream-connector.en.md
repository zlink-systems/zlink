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

    // The last close reason. Empty when it has never disconnected (see "Session close reason").
    Optional<ZLinkStreamCloseReason> closeReason();

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

    // The Actor handles bound right now (common spec §5.6). The application never creates one.
    List<ZLinkStreamActor> actors();
    Optional<ZLinkStreamActor> actor(String actorId);
    AutoCloseable onActorBound(ZLinkStreamActorHandler handler);
    AutoCloseable onActorUnbound(ZLinkStreamActorHandler handler);
}

public interface ZLinkStreamActor {
    String actorId();
    boolean isBound();                          // false after the unbound announcement

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);      // carries this Actor's slot
    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);
    ZLinkTypedStreamSendCall send(Object payload);
    ZLinkTypedStreamSendCall send(String name, Object payload);
    ZLinkTypedStreamRequestCall request(Object payload);
    ZLinkTypedStreamRequestCall request(String name, Object payload);
    AutoCloseable on(String name, ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler); // only messages whose counterpart is this Actor
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

Java exposes an event as an `on...` registration. The meaning is the
same as .NET's event. **Deregistration is done through the returned
`AutoCloseable`'s `close()`, and closing the same value twice isn't
treated as an error**
([Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode)). The
push handler and the error/disconnect/connection state handlers all use
the same return type.

**Session close reason.** The value set and meaning is owned by
[Common Spec §6.2](../../32-stream-connector.en.md#62-close-reason).
Java expresses this as the closed enum `ZLinkStreamCloseReason`
(`CLIENT_CLOSE`, `IDLE_TIMEOUT`, `HEARTBEAT_TIMEOUT`, `SERVER_DRAIN`,
`PROTOCOL_ERROR`, `TRANSPORT_ERROR`). **The read surface is the
connector's `Optional<ZLinkStreamCloseReason> closeReason()` method.**
It returns `Optional.empty()` when the connector has never
disconnected. The disconnect event's `ZLinkStreamCloseReason
closeReason()`, which `ZLinkStreamDisconnectedHandler` receives, is a
surface added on top of that read surface.
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
([04 §1](../../../server/01-execution/README.en.md)).
The Java connector doesn't provide a separate blocking terminator that
waits for the same operation on the current thread. Lifecycle also
follows the same call builder convention, like `connect().submit()`,
`dispatch().submit()`.
The Kotlin wrapper waits for the `CompletionStage` obtained from
`submit()` as a coroutine suspension. This execution meaning follows
the [framework common policy](../../../server/01-execution/README.en.md).

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
    int maxReconnectAttempts,                  // default 3. UNLIMITED_RECONNECT_ATTEMPTS(-1) means unlimited
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
    ZLinkStreamPacketNameResolver nameResolver, // the name resolver injection point of common spec §5.4
    ZLinkStreamTypedCodec typedCodec) { // typed payload codec injection point from common spec §5.4

    // The named constant expressing the unlimited reconnect common spec §6 requires.
    public static final int UNLIMITED_RECONNECT_ATTEMPTS = -1;
}
```

**The option validation timing is owned by
[Common Spec §6.3](../../32-stream-connector.en.md#63-option-validation).**
In Java, `ZLinkStreamConnectorFactory.create(options)` checks every
option, and on a validation failure it builds no `ZLinkStreamConnector`
instance and fails with a `ZLinkStreamException` (§11). A single value
out of range carries `VALIDATION_FAILED`, and a mismatch between
options carries `CONFIGURATION_ERROR`. `maxReconnectAttempts` must be
`UNLIMITED_RECONNECT_ATTEMPTS` or positive.

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

public record ZLinkStreamMessage<TPayload>(
    String packetName,
    TPayload payload,
    Map<String, String> metadata,
    String actorId) { // the counterpart bound Actor; null for a frame without a slot (common spec §5.6)
}
```

The **means of attaching a packet name to a type** that
[Common Spec §5](../../32-stream-connector.en.md#5-packet-model)
requires is an annotation.

```java
@Target(ElementType.TYPE)
@Retention(RetentionPolicy.RUNTIME)
public @interface ZLinkStreamPacketName {
    String value();   // the packet name attached to the type
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
[packet name](../../../server/00-foundation/02-glossary.en.md#packet-name) and a metadata
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

Pending cleanup on request timeout follows
[Common Spec §5.2](../../32-stream-connector.en.md#52-request-correlation).
Java delivers the result through `CompletionStage`.


### 7.1 Request Hooks

The two hooks in [Common Spec §5.7](../../32-stream-connector.en.md#57-request-hooks)
are registered on the connector as
`AutoCloseable onRequestSending(ZLinkStreamRequestSendingHandler)` and
`AutoCloseable onReplyReceived(ZLinkStreamReplyReceivedHandler)` and removed through the
returned value's `close()`.

```java
public interface ZLinkStreamRequestSendingHandler {
    void handle(ZLinkStreamRequestSendingContext context);
}
public interface ZLinkStreamReplyReceivedHandler {
    void handle(ZLinkStreamReplyReceivedContext context);
}
```

`ZLinkStreamRequestSendingContext` exposes `requestPacketName()`, nullable
`actorId()`, and `setMetadata(String key, String value)`. The reply context exposes
read-only `requestPacketName()`, nullable `actorId()`, `succeeded()`,
`reply()` on success, `error()` on failure, and `elapsed()`.
Common Spec §5.7 and §7 govern metadata validation and hook failures.


### 7.2 Test Wait Surface

The contract is owned by
[Common Spec §10.1](../../32-stream-connector.en.md). The Java surface
is below.

**Push observation — connector method** (the same spot as `waitFor`).
Each returns a builder.

Each surface carries **both an overload where the caller states the
packet name and one that decides it from the payload type**
([Common Spec §10.1.1](../../32-stream-connector.en.md#1011-push-observation-surface--the-waitfor-family)).
For the type overload, options' `nameResolver` decides the name.

```java
ZLinkStreamWaitCall       waitFor(String name);              // waits until it arrives
ZLinkStreamWaitCall       waitFor(Class<?> payloadType);
ZLinkStreamExpectNoneCall expectNone(String name);           // whether it doesn't arrive during .within(window)
ZLinkStreamExpectNoneCall expectNone(Class<?> payloadType);
ZLinkStreamSequenceCall   waitForSequence(String name);      // .expect(p).expect(p)… in order
ZLinkStreamSequenceCall   waitForSequence(Class<?> payloadType);

public interface ZLinkStreamExpectNoneCall {
    ZLinkStreamExpectNoneCall within(Duration window);       // the observation window; must be given
    CompletionStage<Void> submit();
}

public interface ZLinkStreamSequenceCall {
    // adds the next predicate to apply in arrival order. Its argument is the message, not the payload.
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

- `expectNone(name).within(Duration).submit()` — **fails with a
  `ZLinkStreamException` carrying `VALIDATION_FAILED`** if it arrives
  within the window. The symmetric of `waitFor`.
- `waitForSequence(name).expect(p1).expect(p2)….timeout(t).submit()` returns
  `List<ZLinkStreamMessage<TPayload>>`; sequence observation and failure follow
  [Common Spec §10.1](../../32-stream-connector.en.md#101-test-wait-surface).
- **The predicate and the return value handle `ZLinkStreamMessage`.**
  The argument `where(...)` and `expect(...)` receive is the message,
  not the payload.
- **A status-only surface isn't provided.** Since status is a payload
  field, it's expressed as
  `waitFor(T.class).where(T.class, m -> m.payload().status() == …)`.

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

The callback execution of `MANUAL` and `IMMEDIATE` and its relationship
to `dispatch().submit()` follow
[Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode).

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
as a closed enum. The **dedicated exception type carrying the code**
that
[Common Spec §9.2](../../32-stream-connector.en.md#92-delivery--the-receiver-must-be-able-to-read-the-code)
requires is `ZLinkStreamException`.

```java
public record ZLinkStreamError(
    ZLinkStreamErrorCode code,   // the closed set of 13 values below
    String message,
    Throwable exception) {       // the causing exception; null when there is none
}

public final class ZLinkStreamException extends RuntimeException {
    public ZLinkStreamException(ZLinkStreamError error);
    public ZLinkStreamError error();           // where the code is read
    public ZLinkStreamErrorCode errorCode();   // shorthand for error().code()
}
```

`ZLinkStreamException` is the only exception the connector throws or
completes a `CompletionStage` with. **A language-standard exception such
as `IllegalArgumentException` or `IllegalStateException` is never thrown
as is** — those types have no place to carry a code, so the caller
cannot tell `VALIDATION_FAILED` from `CONFIGURATION_ERROR`. An option
validation failure (§4) and a wait-surface violation (§7.2) are
delivered through the same exception.

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

## 12. Kotlin Surface

The Kotlin module is a thin wrapper on top of the Java connector. An
operation with a completion value, such as lifecycle and request, is
awaited with the Kotlin wrapper's suspend `await()`. This `await()`
waits for the Java `CompletionStage` as a coroutine suspension. A
one-way send also waits for completion and failure with `await()`, but
doesn't receive a transport result or admission status.

**Kotlin adds no exception hierarchy of its own — it propagates Java's
`ZLinkStreamException` (§11) as is.** When `await()` fails, the same
exception is raised at the call site, and the caller reads the error
code through `error().code()`.

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
    suspend fun await()   // exception if it arrives within the window
}

class ZLinkStreamTypedSequenceCall<TPayload> {
    fun expect(predicate: (ZLinkStreamMessage<TPayload>) -> Boolean): ZLinkStreamTypedSequenceCall<TPayload>
    fun timeout(timeout: Duration): ZLinkStreamTypedSequenceCall<TPayload>
    suspend fun await(): List<ZLinkStreamMessage<TPayload>>   // arrives in predicate order
}

```

**The close reason and the Actor handle are read from the wrapper itself.** Code that uses only
the wrapper must reach them, so it is not left to pull the Java connector out. A Java `Optional`
becomes a Kotlin nullable: the close reason is `null` when the connection has never ended, and
`actor(id)` is `null` for an id that is not bound
([common spec §5.6](../../32-stream-connector.en.md#56-bound-actor)).

**All three wait surfaces offer both the named path and the payload-type
path.** With no name, the name resolution rules for `TPayload` (§5) settle it.
If only `waitFor` has both paths while `expectNone` and `waitForSequence`
demand a name, the three surfaces are called differently inside one test.

The Kotlin wrapper must not build a different state transition or
buffering policy from the Java connector. The extension copying options
**must preserve every option value currently defined.**
A `Flow` surface for a dispatch-mode callback wraps the corresponding Java registration:
the connector's `messages(...)`, `errors()`, `actorBound()` and `actorUnbound()` wrap `on(...)`,
`onErrorReceived(...)`, `onActorBound(...)` and `onActorUnbound(...)`, and an Actor's `messages(...)`
wraps that Actor handle's `ZLinkStreamActor.on(...)`. So in
manual [dispatch mode](../../../server/00-foundation/02-glossary.en.md#dispatch-mode), just
like Java, the Kotlin wrapper's `dispatch().await()` must be called for
the collector to receive a message or error event.

The reply-received hook follows §7 dispatch mode, so Kotlin projects it as
`repliesReceived(): Flow`. Under §5.7, request sending runs synchronously on the
request-calling thread before frame construction, so Kotlin registers it as
`onRequestSending(handler: ZLinkStreamRequestSendingHandler): AutoCloseable`.
The handler mutates metadata synchronously and has no result or cancellation parameter.

## 13. Verification Standard

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
- Request hook order, metadata, failure outcome, and callback error
- Structural inbound flow check with value discarded; outbound flag 0x10 unset
- Both packet-name forms on connector and Actor surfaces
- Kotlin coroutine/Flow wrapper smoke

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](../../../../../../README.en.md) | [Previous: Java STREAM](../../../server/languages/java/interfaces/stream-session.en.md)
<!-- framework-adapter-nav:bottom:end -->
