# Kotlin Stream Connector Usage Guide

The Kotlin Stream Connector wraps the Java connector so it can be used with coroutines and `Flow`.
Transport, reconnect, codec, and the queue are still handled by the Java connector as-is — the
Kotlin wrapper doesn't create a separate network connection.

The exact public contract is owned by the
[Java/Kotlin Stream Connector spec](../../../common/spec/stream-connector/languages/java/03-stream-connector.en.md).
When using the Java `CompletionStage` surface directly, see the
[Java Stream Connector Usage Guide](../../../java/guide/stream-connector/README.en.md).

## 1. Add The Dependency

Specify the ZLink distribution version you're using for `<version>`. Since the Kotlin module
provides the Java connector as a public dependency, the application doesn't declare the connector
module again.

```kotlin
dependencies {
    // Provides the Java connector along with coroutine await, typed wait, and the Flow wrapper.
    implementation("systems.zlink:zlink-framework-kotlin:<version>")
}
```

## 2. Creating A Connector

Create the Java connector, then wrap it with `kotlin()`. The default options use `MANUAL` dispatch,
heartbeat, automatic reconnect, and the JSON typed codec.

```kotlin
import java.net.URI
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions

val connector = ZLinkStreamConnectorFactory
    .create(
        ZLinkStreamConnectorOptions.createDefault(
            URI.create("tcp://127.0.0.1:19000"),
        ),
    )
    .kotlin()

// Suspends the coroutine until connect completes or fails.
connector.connect().await()
```

When shutting down the application, wait for shutdown completion with `connector.close().await()`. A
closed connector can't be connected again.

## 3. Sending And Receiving A Reply With Coroutines

A typed payload uses the default JSON codec. The packet name is decided by an annotation or the type
name; the exact rules and per-call overrides follow the common contract.

```kotlin
import systems.zlink.framework.kotlin.awaitReply

data class LoginRequest(val userId: String)
data class LoginReply(val sessionId: String)
data class PresenceChanged(val userId: String, val online: Boolean)

// Waits until the one-way send finishes or fails.
connector
    .send(PresenceChanged("user-1", true))
    .await()

// The reply type is specified once, as a reified type argument.
val reply: LoginReply = connector
    .request(LoginRequest("user-1"))
    .awaitReply<LoginReply>()
```

For request completions and registered callbacks to run in the default `MANUAL` mode, the
application must repeatedly call `connector.dispatch().await()` from a coroutine or event loop of
its choosing. Bundling the coroutine waiting on the request together with the coroutine performing
dispatch as one sequential job would prevent processing the reply completion, so keep them separate.

```kotlin
suspend fun onApplicationTick() {
    // Call this from an existing UI loop, game loop, or scheduler tick.
    connector.dispatch().await()
}
```

`IMMEDIATE` mode runs the callback directly on the receive path, with no separate dispatch call. If
a handler runs long, both receive and backpressure delivery are delayed together, so use this only
for a short callback.

## 4. Waiting For A Server Push

When waiting for one specific typed push, use `waitFor<T>()`. `where` receives the whole message so
it can inspect both payload and metadata together.

```kotlin
import java.time.Duration

data class MaintenanceNotice(
    val region: String,
    val startsAt: String,
)

val notice = connector
    .waitFor<MaintenanceNotice>()
    .timeout(Duration.ofSeconds(5))
    .where { message -> message.payload.region == "ap-northeast" }
    .await()
```

Since `waitFor` directly pulls a not-yet-consumed received message, no separate dispatch is needed
even in `MANUAL` mode. Don't design a handler and `waitFor` to consume the same packet at the same
time.

## 5. Receiving A Continuous Message Stream With Flow

You can receive a raw message stream arriving by packet name with `messages(...)`, and connector
errors with `errors()`. Canceling the collector also unregisters the internal handler.

```kotlin
import kotlinx.coroutines.flow.collect

connector.messages("MaintenanceNotice").collect { message ->
    // Use this when you need the raw payload. For ordinary business DTOs, prefer typed wait/request.
    println("packet=${message.packetName} flow=${message.flowId}")
}
```

## 6. Operational Checklist

- In `MANUAL` mode, confirm the dispatch coroutine or application tick isn't interrupted.
- The payload size limit and receive queue cap are set in the Java connector options.
- Keep server certificate and hostname verification on in production TLS/WSS.
- When changing reconnect and heartbeat values, review failure-detection time and reconnect load
  together.
- Cancel the Flow collection and wait for connector shutdown when the application scope ends.
