---
title: "Java Binding Guide"
---

<!-- bindings-nav:start -->
[Guide list](../README.en.md) | [Previous: C++](../cpp/index.en.md) | [Next: Node.js](../node/index.en.md)
<!-- bindings-nav:end -->

# Java Binding Guide (`systems.zlink`)

> **Contract-owning document for this chapter** — the [Java bindings spec](../../spec/java/README.en.md)
> covers it. This chapter shows that contract as working sample code.

Explains how to use zlink in Java through working sample code.
The deep explanation of messaging concepts is owned by the
[core guide](https://kairos-code-dev.github.io/zlink/guide/01-overview/); this guide
focuses on using the Java API.

---

## Installation

Add via Gradle or Maven. The native core is bundled per platform.

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

- **Java 22** or later.
- No separate native install — the per-RID shared library loads automatically.

```java
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
```

---

## 5-Minute Example

A minimal example with a `Pair` socket where one side sends `PING` and the other
replies with `ACK`. Every resource is managed with `try-with-resources`.

```java
// Server
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
// Client
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

## Core Types

The 4 fundamental types every feature shares.

### 1. Context

The runtime entry point for a process. Implements `AutoCloseable`, so it's
managed with try-with-resources. Closing the context interrupts blocking
operations on its child sockets/services.

```java
try (Context ctx = Zlink.createContext()) {
    // create sockets and services here
    var socket = ctx.createPairSocket();
    // ...
} // ctx.close() runs automatically → child sockets shut down
```

Adjust the I/O thread count:

```java
ctx.options().ioThreads(4);
```

### 2. Message

Owns a single payload frame. Implements `AutoCloseable`. Sending transfers
ownership, so it doesn't need a separate close. If the send fails, ownership is
retained, so retry or close it explicitly.

```java
// build a copy from a string
try (Message msg = Message.from("payload")) {
    socket.send().message(msg).submit();
}
// once submit succeeds msg is already consumed — closing the try block is harmless

// build a copy from a byte array
try (Message msg = Message.from(bytes)) { ... }

// allocate a sized empty frame
try (Message msg = new Message(256)) {
    msg.mutableDataBuffer().put(data);
    socket.send().message(msg).submit();
}
```

Calling `recv(..., RecvFlags.NONE)` directly blocks the current Java thread in
native recv. This surface is a low-level socket API. On a framework path
handling many sessions/handlers, don't put this call directly on a handler
thread — wait for readiness with a `Poller`, then do a `RecvFlags.DONT_WAIT`
recv on the ready socket. Application handlers run behind the handler executor
the framework configures.

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

Reading a received message:

```java
int size = msg.size();
String text = msg.toUtf8String();    // UTF-8 conversion
byte[] data = msg.data();            // copy into a byte array
ByteBuffer buf = msg.dataBuffer();   // read-only view
```

### 3. Received — the receive envelope

Holds a received message envelope. Carries a routing ID, part list, and an
optional reply context. Reusable. Implements `AutoCloseable`.

```java
try (Received received = new Received()) {
    socket.recv(received, RecvFlags.NONE);

    // single-part access
    Message part = received.firstPart();         // first part
    Message part = received.singlePartOrThrow();  // must be exactly one part

    // multipart access
    List<Message> parts = received.parts();

    // routing ID (present on ROUTER/SPOT receive)
    Optional<RoutingId> rid = received.getRoutingId();
}
```

### 4. RoutingId

An immutable value of 1-255 bytes identifying a peer or spot.

```java
RoutingId rid = RoutingId.from("server-01".getBytes(StandardCharsets.UTF_8));
RoutingId rid = RoutingId.from("server-01");
```

---

## Ownership And Lifetime

The Java binding's ownership rules. try-with-resources is the default pattern.

| Situation | Rule |
|------|------|
| `submit()` succeeds | ownership of the added `Message` transfers to the send stack. No separate `close()` needed |
| `submit()` fails (exception) | ownership is retained by the caller. try-with-resources handles it automatically |
| `recv()` succeeds | the caller owns the `Received`. try-with-resources required |
| `submitAsync()` completes | the reply `List<Message>` is caller-owned. Needs `Message.closeAll(reply)` |
| `Context.close()` | interrupts every blocking operation under the context |

```java
// pattern: safe via try-with-resources
try (Message msg = Message.from("data")) {
    boolean submitted = socket.send().message(msg).submit();
    // submitted=true means msg was consumed; false means back-pressure (only with DONT_WAIT)
} // if submit throws, try-with-resources closes msg
```

---

## Error Handling

The Java binding throws exceptions from the `ZlinkException` hierarchy.

```java
try (Message msg = Message.from("data")) {
    socket.send().message(msg).submit();
} catch (ZlinkSubmitException e) {
    switch (e.getResult()) {
        case BACKPRESSURED -> { /* retry shortly */ }
        case NOT_CONNECTED -> { /* no connected peer */ }
        default -> throw e;
    }
}
```

Exception types:

| Exception class | Raised when | Result field |
|------------|----------|-----------|
| `ZlinkSubmitException` | send/publish failure | `getResult(): SubmitResult` |
| `ZlinkRequestException` | request failure | `getResult(): RequestResult` |
| `ZlinkRecvException` | receive failure | `getResult(): RecvResult` |
| `ZlinkBindException` | bind failure | `getResult(): BindResult` |
| `ZlinkConnectException` | connect failure | `getResult(): ConnectResult` |
| `ZlinkConfigException` | option-set failure | `getResult(): ConfigResult` |
| `ZlinkCloseException` | close failure | `getResult(): CloseResult` |
| `ZlinkHandlerException` | handler registration failure | `getResult(): HandlerResult` |

Every exception inherits from `ZlinkException` and exposes `getCode()` and
`getInternalErrno()` to check the native code.

---

## C API Mapping

| C API | Java API |
|-------|----------|
| `zlink_ctx_new()` | `Zlink.createContext()` |
| `zlink_ctx_term()` | `ctx.close()` |
| `zlink_socket(ctx, type)` | `ctx.createPairSocket()`, etc. |
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

## Native Library / Deployment

The Java binding embeds a per-platform shared library. No separate install —
just add it via Gradle/Maven.

Checking the native version in use:

```java
int[] version = Zlink.version();
System.out.printf("zlink %d.%d.%d%n", version[0], version[1], version[2]);
```

Checking whether a specific feature is supported:

```java
if (Zlink.has("draft")) {
    System.out.println("draft API supported");
}
```

**Threading:** `Context` can be shared across threads, but sockets must be used
**from a single thread only**. Dispatch handlers are invoked on zlink's internal
worker threads, so avoid blocking for long inside a handler. See
[thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/) for
details.

---

## Samples

Verified sample code lives at
`bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/`.

| Sample class | Description |
|------------|------|
| `PairRecvSample` | PAIR socket send/receive |
| `DealerRouterRecvSample` | DEALER/ROUTER send/receive |
| `RequestReplyAsyncSample` | Async request/reply |
| `PubSubRecvSample` | PUB/SUB publish/subscribe |
| `StreamRecvSample` | STREAM raw TCP |
| `StreamPacketCallbackSample` | STREAM packet callback |
| `MonitorRecvSample` | Monitor event receive |

> SPOT/Actor examples are covered by the framework samples, not the core
> binding — see the Spot/Actor links under [See Also](#see-also) below.

Building and running the samples:

```bash
cd bindings/java
./gradlew :samples:build
./gradlew :samples:run -PmainClass=systems.zlink.samples.PairRecvSample
```

---

## Kotlin

Kotlin uses the Java binding (`systems.zlink.*`) **as-is, with no separate
native binding**. The installation, core types, ownership, errors, and mapping
table above all apply identically — only the idiom differs for Kotlin.

- **Dependency**: `systems.zlink:zlink` (same as above). Use Kotlin plugin
  **2.1.0** or later.
- **Ownership**: since it's `AutoCloseable`, clean up with `use { }` instead of
  `try`/`finally`.

```kotlin
Zlink.createContext().use { ctx ->
    ctx.createPairSocket().use { socket ->
        socket.bind("tcp://127.0.0.1:5555")
        // ...
    }
}
```

- **Callbacks**: handlers pass through as Kotlin lambdas as-is —
  `timer.onFire { _, n -> ... }`.
- **Samples**: `bindings/kotlin/samples/` (`.kt`) has the same canonical set as
  the Java samples. Build/run through the Java gradle `:kotlin-samples`
  subproject.

```bash
cd bindings/java
./gradlew :kotlin-samples:runPairRecvSample --no-daemon
```

The core guide's language tabs have a dedicated **Kotlin** column, so you can
see messaging/service usage directly in Kotlin code.

---

## See Also

**Socket patterns**
- [Socket pattern overview](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  - [PAIR](https://kairos-code-dev.github.io/zlink/guide/03-1-pair/)
  - [PUB/SUB](https://kairos-code-dev.github.io/zlink/guide/03-2-pubsub/)
  - [DEALER](https://kairos-code-dev.github.io/zlink/guide/03-3-dealer/)
  - [ROUTER](https://kairos-code-dev.github.io/zlink/guide/03-4-router/)
  - [STREAM](https://kairos-code-dev.github.io/zlink/guide/03-5-stream/)
  - [Proxy](https://kairos-code-dev.github.io/zlink/guide/03-6-proxy/)

**Services**
- [Framework service overview](../../../../framework/doc/framework/common/guide/server/03-concepts.en.md)
  - [Spot](../../../../framework/doc/framework/common/guide/server/06-spot.en.md)
  - [Actor](../../../../framework/doc/framework/common/guide/server/07-actor-spot.en.md)

**Operations**
- [Socket options](https://kairos-code-dev.github.io/zlink/guide/12-socket-options/)
- [TLS security](https://kairos-code-dev.github.io/zlink/guide/05-tls-security/)
- [Monitoring](https://kairos-code-dev.github.io/zlink/guide/06-monitoring/)
- [Thread safety](https://kairos-code-dev.github.io/zlink/guide/11-thread-safety/)
- [Message API](https://kairos-code-dev.github.io/zlink/guide/09-message-api/)
- [Routing ID](https://kairos-code-dev.github.io/zlink/guide/08-routing-id/)
