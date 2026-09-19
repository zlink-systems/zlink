# Installation and the First Connection

!!! info "After reading this chapter"

    You can add the connector package to a project, connect to a server, and exchange the first
    packet. The connection code in this chapter runs as it stands in
    `framework/languages/<language>/tutorial/StreamClient`.

The connector ships separately from the server framework, so a client project references the
connector package alone. This chapter walks from installation to the first reply. The full set of
options and their defaults is covered by [Connector Options](03-connector-options.en.md).

## 1. Installation

=== "C++"

    ```bash
    vcpkg install "zlink-stream-connector[tls,websocket]"
    ```

    ```cmake
    find_package(zlink-stream-connector CONFIG REQUIRED)

    target_link_libraries(my_game PRIVATE zlink::stream_connector)
    ```

=== "C#/.NET"

    ```bash
    dotnet add package Zlink.Stream.Connector
    ```

=== "Java"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-stream-connector:0.17.0")
    }
    ```

=== "Kotlin"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-stream-connector:0.17.0")
        // Coroutine wrapper. The surfaces that suspend with await() live in this module.
        implementation("systems.zlink:zlink-framework-kotlin:0.17.0")
    }
    ```

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/stream-connector
    ```

## 2. Connecting and the First Request

A connector is created from options that carry the endpoint and the timeouts. Packets can be sent
only after the connection is established, so the connection is awaited first. A request waits
until the server's answer arrives and then returns the answer payload.

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:stream-client"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:stream-client"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:stream-client"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:stream-client"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:stream-client"
    ```

!!! warning "A browser client connects over `ws://`"

    Native builds use `tcp://`, `tls://`, `ws://`, and `wss://`. A browser runtime cannot open an
    OS socket, so it uses `ws://` or `wss://` only, and the server endpoint carries the same
    scheme.

## 3. What You See

The command below runs the tutorial (`framework/languages/<language>/tutorial`) StreamClient with its Server started as described in the README's "Run" section; StreamClient connects to the Server's stream endpoint.

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

=== "C++"

    ```cpp
    connector.send (chat_message_t{"room-42", "hello"})
      .packet_name ("chat.send")   // Omitted, the name comes from the payload type.
      .submit ();
    ```

=== "C#/.NET"

    ```csharp
    await connector.Send(new ChatMessage("room-42", "hello"))
        .PacketName("chat.send")   // Omitted, the name comes from the payload type.
        .Async();
    ```

=== "Java"

    ```java
    connector.send(new ChatMessage("room-42", "hello"))
        .packetName("chat.send")   // Omitted, the name comes from the payload type.
        .submit()
        .toCompletableFuture()
        .join();
    ```

=== "Kotlin"

    ```kotlin
    // val connector = ZLinkStreamConnectorFactory.create(options).kotlin()
    connector.send(ChatMessage("room-42", "hello"))
        .packetName("chat.send")   // Omitted, the name comes from the payload type.
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    await connector
      .send(new ChatMessage('room-42', 'hello'))
      .packetName('chat.send')   // Omitted, the name comes from the payload constructor.
      .submit();
    ```

## 5. Receiving What the Server Sends First

Packets the server sends on its own are received by a registered handler. Registration returns a
value that can be released, and the handler stays in place while that value lives. Under the
default setting a handler does not run at receive time: it runs in the execution context that
called the pump. A game loop calls the pump once per frame.

=== "C++"

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

=== "C#/.NET"

    ```csharp
    using var subscription = connector.On<LeaderboardUpdate>((message, cancellationToken) =>
    {
        Console.WriteLine(message.Payload!.Rank);
        return ValueTask.CompletedTask;
    });

    while (running)
    {
        await connector.Dispatch.Async();   // Runs the handlers queued so far and returns.
        RenderFrame();
    }
    ```

=== "Java"

    ```java
    AutoCloseable subscription = connector.on(LeaderboardUpdate.class, message -> {
        System.out.println(message.payload().rank());
        return CompletableFuture.completedFuture(null);
    });

    while (running) {
        connector.dispatch().submit().toCompletableFuture().join();
        renderFrame();
    }
    ```

=== "Kotlin"

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

=== "Node/TypeScript"

    ```typescript
    const subscription = connector.on<LeaderboardUpdate>(
      'leaderboard.update',
      message => { console.log(message.payload.rank); },
      LeaderboardUpdate
    );

    await connector.dispatch();   // Runs the handlers queued so far and returns.
    ```

## 6. Next Chapters

- Every option and its default — [Connector Options](03-connector-options.en.md)
- Packet names, metadata, compression — [Sending Packets](04-sending.en.md)
- The receive queue and the wait surfaces — [Receiving Packets](05-receiving.en.md)
