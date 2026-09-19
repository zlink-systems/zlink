---
title: "Installation and the First Connection · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/02-getting-started.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Installation and the First Connection

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Stream Connector Overview](01-overview.en.md) | [Next: Connector Options](03-connector-options.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/stream-connector/02-getting-started.en.md) · [Java](../../../java/guide/stream-connector/02-getting-started.en.md) · [Kotlin](../../../kotlin/guide/stream-connector/02-getting-started.en.md) · [Node/TypeScript](../../../node/guide/stream-connector/02-getting-started.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can add the connector package to a project, connect to a server, and exchange the first
    packet. The connection code in this chapter runs as it stands in
    `framework/languages/cpp/tutorial/StreamClient`.

The connector ships separately from the server framework, so a client project references the
connector package alone. This chapter walks from installation to the first reply. The full set of
options and their defaults is covered by [Connector Options](03-connector-options.en.md).

## 1. Installation

```bash
vcpkg install "zlink-stream-connector[tls,websocket]"
```

```cmake
find_package(zlink-stream-connector CONFIG REQUIRED)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

## 2. Connecting and the First Request

A connector is created from options that carry the endpoint and the timeouts. Packets can be sent
only after the connection is established, so the connection is awaited first. A request waits
until the server's answer arrives and then returns the answer payload.

```cpp
--8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:stream-client"
```

!!! warning "A browser client connects over `ws://`"

    Native builds use `tcp://`, `tls://`, `ws://`, and `wss://`. A browser runtime cannot open an
    OS socket, so it uses `ws://` or `wss://` only, and the server endpoint carries the same
    scheme.

## 3. What You See

The command below runs the tutorial (`framework/languages/cpp/tutorial`) StreamClient with its Server started as described in the README's "Run" section; StreamClient connects to the Server's stream endpoint.

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

```cpp
connector.send (chat_message_t{"room-42", "hello"})
  .packet_name ("chat.send")   // Omitted, the name comes from the payload type.
  .submit ();
```

## 5. Receiving What the Server Sends First

Packets the server sends on its own are received by a registered handler. Registration returns a
value that can be released, and the handler stays in place while that value lives. Under the
default setting a handler does not run at receive time: it runs in the execution context that
called the pump. A game loop calls the pump once per frame.

```cpp
auto subscription = connector.on<leaderboard_update_t> (
  [] (const sc::message_t<leaderboard_update_t> &message) {
      std::cout << message.payload.rank << std::endl;
  });

while (running) {
    connector.dispatch ();   // Runs the handlers queued so far and returns.
    render_frame ();
}
```

## 6. Next Chapters

- Every option and its default — [Connector Options](03-connector-options.en.md)
- Packet names, metadata, compression — [Sending Packets](04-sending.en.md)
- The receive queue and the wait surfaces — [Receiving Packets](05-receiving.en.md)
