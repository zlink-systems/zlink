---
title: "Installation and the First Connection · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/02-getting-started.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Installation and the First Connection

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Stream Connector Overview](01-overview.en.md) | [Next: Connector Options](03-connector-options.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/02-getting-started.en.md) · [C#/.NET](../../../dotnet/guide/stream-connector/02-getting-started.en.md) · [Java](../../../java/guide/stream-connector/02-getting-started.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/stream-connector/02-getting-started.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can add the connector package to a project, connect to a server, and exchange the first
    packet. The connection code in this chapter runs as it stands in
    `framework/languages/<language>/tutorial/StreamClient`.

The connector ships separately from the server framework, so a client project references the
connector package alone. This chapter walks from installation to the first reply. The full set of
options and their defaults is covered by [Connector Options](03-connector-options.en.md).

## 1. Installation

```kotlin
dependencies {
    implementation("systems.zlink:zlink-stream-connector:0.17.0")
    // Coroutine wrapper. The surfaces that suspend with await() live in this module.
    implementation("systems.zlink:zlink-framework-kotlin:0.17.0")
}
```

## 2. Connecting and the First Request

A connector is created from options that carry the endpoint and the timeouts. Packets can be sent
only after the connection is established, so the connection is awaited first. A request waits
until the server's answer arrives and then returns the answer payload.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:stream-client"
```

!!! warning "A browser client connects over `ws://`"

    Native builds use `tcp://`, `tls://`, `ws://`, and `wss://`. A browser runtime cannot open an
    OS socket, so it uses `ws://` or `wss://` only, and the server endpoint carries the same
    scheme.

## 3. What You See

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
```

A true `connected` means the connection is established. The round trip is measured from the time
the client sent, which the server echoes back.

## 4. Sending Without an Answer

A packet that needs no answer is sent with send. Nothing goes out until the terminal call is made,
and that terminal reports completion and failure only. Use a request when the client needs to know
what the server did.

```kotlin
// val connector = ZLinkStreamConnectorFactory.create(options).kotlin()
connector.send(ChatMessage("room-42", "hello"))
    .packetName("chat.send")   // Omitted, the name comes from the payload type.
    .await()
```

## 5. Receiving What the Server Sends First

Packets the server sends on its own are received by a registered handler. Registration returns a
value that can be released, and the handler stays in place while that value lives. Under the
default setting a handler does not run at receive time: it runs in the execution context that
called the pump. A game loop calls the pump once per frame.

```kotlin
val subscription = connector.on<LeaderboardUpdate> { message ->
    println(message.payload.rank)
    CompletableFuture.completedFuture(null)
}

while (running) {
    connector.dispatch().await()   // Runs the handlers queued so far and returns.
    renderFrame()
}
```

## 6. Next Chapters

- Every option and its default — [Connector Options](03-connector-options.en.md)
- Packet names, metadata, compression — [Sending Packets](04-sending.en.md)
- The receive queue and the wait surfaces — [Receiving Packets](05-receiving.en.md)
