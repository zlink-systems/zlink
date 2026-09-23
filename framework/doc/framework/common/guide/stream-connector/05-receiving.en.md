# Receiving Packets

!!! info "After reading this chapter"

    You can receive packets the server sends first through a handler and release that registration
    when you choose. You can also wait for a single packet and read how many arrived under a name.

A packet the server sent stays in the **receive queue** until a handler takes it or a wait surface
consumes it. This chapter covers how packets leave that queue. Answers and heartbeats do not pass
through it — an answer goes straight to the request waiting for it, and a heartbeat serves the
connection itself.

## 1. Registering a Handler

A handler receives a **message**, not a payload alone. The message carries the packet name, the
decoded payload, the metadata, and the flow identifier. Which packets it receives is decided by the
payload type or by an explicit name.

=== "C++"

    ```cpp
    auto subscription = connector.on<leaderboard_update_t> (
      [] (const sc::message_t<leaderboard_update_t> &message) {
          update_board (message.packet_name, message.payload.rank);
      });
    ```

=== "C#/.NET"

    ```csharp
    using var subscription = connector.On<LeaderboardUpdate>((message, cancellationToken) =>
    {
        UpdateBoard(message.Name, message.Payload!.Rank);
        return ValueTask.CompletedTask;
    });
    ```

=== "Java"

    ```java
    AutoCloseable subscription = connector.on(LeaderboardUpdate.class, message -> {
        updateBoard(message.packetName(), message.payload().rank());
        return CompletableFuture.completedFuture(null);
    });
    ```

=== "Kotlin"

    ```kotlin
    val subscription = connector.on<LeaderboardUpdate> { message ->
        updateBoard(message.packetName, message.payload.rank)
        CompletableFuture.completedFuture(null)
    }
    ```

=== "Node/TypeScript"

    ```typescript
    // A TypeScript type does not survive to run time, so the name and the constructor are given.
    const subscription = connector.on<LeaderboardUpdate>(
      'leaderboard.update',
      message => { updateBoard(message.name, message.payload.rank); },
      LeaderboardUpdate
    );
    ```

A handler may send again over the same connector. That send continues the flow of the message
being handled, so client logs and server traces line up on one flow.

## 2. Releasing a Registration

Registration returns **a value that can be released**. This keeps a client that registers a
subscription for the lifetime of one screen from having to rebuild the connection when the screen
closes. A released handler does not run afterwards, and releasing the same value twice is not an
error.

=== "C++"

    ```cpp
    subscription.unsubscribe ();   // The registration also ends when the value goes away.
    ```

=== "C#/.NET"

    ```csharp
    subscription.Dispose();
    ```

=== "Java"

    ```java
    subscription.close();
    ```

=== "Kotlin"

    ```kotlin
    subscription.close()
    ```

=== "Node/TypeScript"

    ```typescript
    subscription.dispose();
    ```

**Whether the value's lifetime is the registration's lifetime is decided by the language.** In a
language that expresses ownership as a value, the registration ends when the value goes away, so
the value is kept for as long as the handler must run. In the others the registration stays until it
is released explicitly, even if the value is discarded.

The connection-state, error, and disconnect handlers return the same kind of value. They are
covered by [Connection Lifecycle](06-lifecycle.en.md).

## 3. When a Handler Runs

Under the default setting the receive path does not call handlers directly; it queues them. When
the application calls the pump, the handlers queued so far run **in the execution context that
called it**. A game loop calls it once per frame. The pump processes what has accumulated and
returns; it does not wait for a new packet.

Switching to immediate execution runs handlers on the receive path with no pump. A slow handler then
blocks that path and delays the receive work behind it.

=== "C++"

    ```cpp
    while (running) {
        connector.dispatch ();
        render_frame ();
    }
    ```

=== "C#/.NET"

    ```csharp
    while (running)
    {
        await connector.Dispatch.Async();
        RenderFrame();
    }
    ```

=== "Java"

    ```java
    while (running) {
        connector.dispatch().submit().toCompletableFuture().join();
        renderFrame();
    }
    ```

=== "Kotlin"

    ```kotlin
    while (running) {
        connector.dispatch().await()
        renderFrame()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    while (running) {
      await connector.dispatch();
      renderFrame();
    }
    ```

The wait surfaces below observe the receive queue directly rather than running registered handlers,
so they work in a configuration that never calls the pump.

## 4. Waiting for One Packet

To wait for a single packet at a point in a scenario, use a wait surface instead of registering a
handler. It consumes the matching packet and returns that message; a packet that does not match
stays in the queue for a later handler or wait. Without an explicit timeout, the connector's default
wait timeout applies.

=== "C++"

    ```cpp
    auto found = connector.wait_for<match_found_t> ()
                   .where ([] (const auto &message) {
                       return message.payload.match_id == "match-7f3a";
                   })
                   .timeout (std::chrono::seconds (30))
                   .submit ();
    ```

=== "C#/.NET"

    ```csharp
    var found = await connector.WaitFor<MatchFound>()
        .Where(message => message.Payload!.MatchId == "match-7f3a")
        .Timeout(TimeSpan.FromSeconds(30))
        .Async();
    ```

=== "Java"

    ```java
    ZLinkStreamMessage<MatchFound> found = connector.waitFor(MatchFound.class)
        .where(MatchFound.class, message -> "match-7f3a".equals(message.payload().matchId()))
        .timeout(Duration.ofSeconds(30))
        .submit(MatchFound.class)
        .toCompletableFuture()
        .join();
    ```

=== "Kotlin"

    ```kotlin
    val found = connector.waitFor<MatchFound>()
        .where { it.payload.matchId == "match-7f3a" }
        .timeout(Duration.ofSeconds(30))
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    const found = await connector
      .waitFor<MatchFound>('match.found')
      .where(message => message.payload.matchId === 'match-7f3a')
      .timeout(30_000)
      .submit();
    ```

**Predicates and returns deal in messages, not payloads.** A predicate given the payload alone
cannot see the packet name or the metadata. When the value to filter on sits inside the payload,
read that field inside the predicate — there is no surface dedicated to a status field.

## 5. Packets That Must Not Arrive, and Arrival Order

Verifying a scenario means confirming not only that a packet arrived, but also that one did not and
that several arrived in order. The first names an observation window and confirms the packet does
not arrive during it. The second applies predicates in order, confirms packets of the same name
arrived in that order, and returns the list of messages.

=== "C++"

    ```cpp
    auto quiet = connector.expect_none<order_changed_t> ()
                   .within (std::chrono::milliseconds (100))
                   .submit ();

    auto steps = connector.wait_for_sequence<order_changed_t> ()
                   .expect ([] (const auto &m) { return m.payload.status == status_t::paid; })
                   .expect ([] (const auto &m) { return m.payload.status == status_t::shipped; })
                   .timeout (std::chrono::seconds (2))
                   .submit ();
    ```

=== "C#/.NET"

    ```csharp
    await connector.ExpectNone<OrderChanged>()
        .Within(TimeSpan.FromMilliseconds(100))
        .Async();

    var steps = await connector.WaitForSequence<OrderChanged>()
        .Expect(message => message.Payload!.Status == OrderStatus.Paid)
        .Expect(message => message.Payload!.Status == OrderStatus.Shipped)
        .Timeout(TimeSpan.FromSeconds(2))
        .Async();
    ```

=== "Java"

    ```java
    connector.expectNone(OrderChanged.class)
        .within(Duration.ofMillis(100))
        .submit()
        .toCompletableFuture()
        .join();

    List<ZLinkStreamMessage<OrderChanged>> steps = connector.waitForSequence(OrderChanged.class)
        .expect(OrderChanged.class, m -> m.payload().status() == OrderStatus.PAID)
        .expect(OrderChanged.class, m -> m.payload().status() == OrderStatus.SHIPPED)
        .timeout(Duration.ofSeconds(2))
        .submit(OrderChanged.class)
        .toCompletableFuture()
        .join();
    ```

=== "Kotlin"

    ```kotlin
    connector.expectNone<OrderChanged>("order.changed")
        .within(Duration.ofMillis(100))
        .await()

    val steps = connector.waitForSequence<OrderChanged>("order.changed")
        .expect { it.payload.status == OrderStatus.PAID }
        .expect { it.payload.status == OrderStatus.SHIPPED }
        .timeout(Duration.ofSeconds(2))
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    await connector.expectNone<OrderChanged>('order.changed').within(100).run();

    const steps = await connector
      .waitForSequence<OrderChanged>('order.changed')
      .expect(message => message.payload.status === 'paid')
      .expect(message => message.payload.status === 'shipped')
      .timeout(2_000)
      .run();
    ```

A failed observation — nothing arrived in time, something arrived that should not have, the order
was wrong — is reported as a validation failure. A wait that cannot continue because the connection
ended is reported as no connection. The caller has to tell an observation that did not hold from an
observation that lost its subject.

## 6. Received Counts

The count of packets **received** under a name is readable per name. Consuming one does not lower
it, so the count still answers how many arrived under that name after a handler has run through the
pump. The count also rises when the packet arrives, whatever the setting for when handlers run.

=== "C++"

    ```cpp
    auto count = connector.received_count ("leaderboard.update");
    ```

=== "C#/.NET"

    ```csharp
    var count = connector.ReceivedCount("leaderboard.update");
    ```

=== "Java"

    ```java
    int count = connector.receivedCount("leaderboard.update");
    ```

=== "Kotlin"

    ```kotlin
    val count = connector.receivedCount("leaderboard.update")
    ```

=== "Node/TypeScript"

    ```typescript
    const count = connector.receivedCount('leaderboard.update');
    ```

The reference point is the moment the connection is established. The count starts at zero then, and
a reconnect is a new connection, so it starts at zero again. Unconsumed messages left from the
previous connection are cleared at the same moment — otherwise a wait surface would hand back a
packet from before the drop as if it belonged to the new connection.

## 7. The Receive Queue — No Limit Is Set

The connector keeps receiving and processing what arrives. There is no limit on the queue, no
message is dropped, and a long queue is never a reason to close the connection. The connector does
not implement a socket of its own and uses what the runtime provides, so there is no place to hold
reads back either.

In a client that works correctly, messages do not accumulate: a handler processes them or a wait
surface consumes them. Continued growth means the client is not calling the pump, which is why the
received count is not a basis for flow control.

## 8. Sending and Receiving with an Actor Handle

An application with one Actor needs no changes to its existing send and receive code. When the server binds several Actors to one connection, use a handle to choose the Actor for a send and read the received message’s Actor ID to identify its server-side counterpart. When the server binds an Actor to this connection, the bound notice arrives before that Actor's first packet; when the binding ends, the unbound notice arrives after its last packet.

Register for bound and unbound notices first. The tutorial server then binds `p1`; the client looks up its handle, sends through it, and reads the Actor ID on the returned message.

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:actor-handle-events"
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:actor-handle-send"
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:actor-handle-send-call"
    --8<-- "framework/languages/cpp/tutorial/StreamClient/main.cpp:actor-handle-receive"
    ```

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-events"
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-send"
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-send-call"
    --8<-- "framework/languages/dotnet/tutorial/StreamClient/Program.cs:actor-handle-receive"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:actor-handle-events"
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:actor-handle-send"
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:actor-handle-send-call"
    --8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:actor-handle-receive"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:actor-handle-events"
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:actor-handle-send"
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:actor-handle-send-call"
    --8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:actor-handle-receive"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:actor-handle-events"
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:actor-handle-send"
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:actor-handle-send-call"
    --8<-- "framework/languages/node/tutorial/StreamClient/main.ts:actor-handle-receive"
    ```

The output includes `actor handle: p1` and `pushed: speedy, actor: p1`.

## 9. Next Chapters

- Connection state, reconnection, close reasons — [Connection Lifecycle](06-lifecycle.en.md)
- Errors raised on the receive path — [Error Handling](07-error-handling.en.md)
