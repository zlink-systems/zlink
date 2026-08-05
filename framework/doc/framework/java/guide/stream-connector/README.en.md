# Java Stream Connector Usage Guide

The Java Stream Connector is used when a JVM client connects to a ZLink Framework STREAM endpoint.
It's a `systems.zlink:zlink-stream-connector` module independent of the server framework, and it can
also be used from tools, E2E clients, or bots that don't have Spring Boot.

The public types and exact defaults are owned by the
[Java/Kotlin Stream Connector spec](../../../common/spec/stream-connector/languages/java/03-stream-connector.en.md).
Transport and wire behavior follow the
[Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md).

## 1. Add The Dependency

Specify the ZLink distribution version you're using for `<version>`.

```kotlin
dependencies {
    // Provides the Java connector itself and TCP/TLS/WS/WSS transport.
    implementation("systems.zlink:zlink-stream-connector:<version>")
}
```

## 2. Connect And Terminate

The endpoint URI's scheme decides the transport. You can use `tcp`, `tls`, `ws`, `wss`.
`createDefault(...)` configures `MANUAL` dispatch, heartbeat, automatic reconnect, and the JSON
typed codec.

```java
import java.net.URI;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;

var options = ZLinkStreamConnectorOptions.createDefault(
    URI.create("tcp://127.0.0.1:19000"));

ZLinkStreamConnector connector =
    ZLinkStreamConnectorFactory.create(options);

// Connection completion or failure is delivered as a CompletionStage.
connector.connect().submit().toCompletableFuture().join();

try {
    // Perform application work.
} finally {
    // Once close completes, you can't connect again with the same connector.
    connector.close().submit().toCompletableFuture().join();
}
```

TLS and WSS verify the server certificate and hostname by default. The option to skip certificate
verification is used only in tests with a self-signed certificate.

## 3. Sending A Typed Message And Requests

The default codec is JSON. If the payload type has `@ZLinkStreamPacketName`, that value is used as
the packet name; otherwise, the class's simple name is used. Specifying a per-call `packetName(...)`
takes priority over that.

```java
record LoginRequest(String userId) {}
record LoginReply(String sessionId) {}
record PresenceChanged(String userId, boolean online) {}

// A one-way send's stage delivers only send completion or failure.
connector.send(new PresenceChanged("user-1", true))
    .metadata("operationId", "presence-user-1-online")
    .submit();

// A request's reply type is specified in submit.
var login = connector.request(new LoginRequest("user-1"))
    .metadata("operationId", "login-20260730-001")
    .submit(LoginReply.class);

login.thenAccept(reply -> {
    // Reflect reply.sessionId() into application state.
});
```

Put small values like an operation ID or tracing value in metadata. Put large business data in the
payload. To change the request timeout per call, call `timeout(Duration)` before `submit(...)`.

## 4. Handling Received Messages

In the default `MANUAL` mode, network receive doesn't run handlers directly. The application must
periodically call `dispatch().submit()` on a thread of its choosing for the registered push
handlers, request completions, and disconnect/error handlers to run.

```java
var registration = connector.on(
    PresenceChanged.class,
    message -> {
        PresenceChanged payload = message.payload();
        // This callback runs on the thread the application called dispatch from.
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    });

// Call this repeatedly from a UI loop, game loop, or application scheduler.
connector.dispatch().submit();

// Unregister the handler once it's no longer needed.
registration.close();
```

`IMMEDIATE` mode runs the handler directly on the receive path. No separate dispatch call is
needed, but a slow handler delays receive, so use this only when the handler is short enough.

When waiting for one specific server push, use `waitFor(...)`. Since this wait feature directly
consumes a not-yet-processed received message, no separate dispatch call is needed even in `MANUAL`
mode.

```java
var notice = connector.waitFor("MaintenanceNotice")
    .timeout(java.time.Duration.ofSeconds(5))
    .submit();
```

## 5. Operational Checklist

- Confirm the payload doesn't exceed `maxSendPayloadSize` and `maxReceivePayloadSize`.
- In `MANUAL` mode, confirm dispatch calls aren't interrupted.
- When changing reconnect and heartbeat defaults, review failure-detection time and reconnect load
  together.
- Keep certificate verification on in production TLS/WSS.
- Wait for `close().submit()` to complete on process shutdown.

To use Kotlin coroutines and `Flow`, see the
[Kotlin Stream Connector Usage Guide](../../../kotlin/guide/stream-connector/README.en.md).
