# Connection Lifecycle

!!! info "After reading this chapter"

    You can read the connection state and receive state changes through handlers, you know how far
    automatic reconnection goes, and you can find out why a connection ended.

One connector represents one connection. The connection is created, connected, dropped,
reconnected, and closed, and a closed connector is never connected again. This chapter covers what
the application has to observe and handle along the way.

## 1. Connection States

| State | Meaning |
|---|---|
| Created | The connector exists but no connection has been started |
| Connecting | The initial connection is in progress |
| Connected | The connection is established and packets can be exchanged |
| Reconnecting | Automatic reconnection is in progress |
| Disconnected | The transport connection dropped |
| Closed | The connector is closed and is never connected again |

**Created and disconnected are different states.** Telling "never connected" from "was connected and
dropped" changes the reconnection decision.

## 2. Connecting and Closing

The connect call completes once the connection and the receive path are ready. The close call closes
the transport without writing frames that have not gone out, and fails their Sends and Requests,
together with the requests waiting for a reply, with `Disconnected`. To know that a Send's frame was
written, wait for that Send to complete before closing.

=== "C++"

    ```cpp
    auto connected = connector.connect ();
    if (!connected) {
        return;   // The failure code is read from connected.error_code ().
    }

    connector.close ();
    ```

=== "C#/.NET"

    ```csharp
    await connector.Connect.Async();

    await connector.Close.Async();
    ```

=== "Java"

    ```java
    connector.connect().submit().toCompletableFuture().join();

    connector.close().submit().toCompletableFuture().join();
    ```

=== "Kotlin"

    ```kotlin
    connector.connect().await()

    connector.close().await()
    ```

=== "Node/TypeScript"

    ```typescript
    await connector.connect();

    await connector.close();
    ```

A connect call behaves differently depending on the current state.

| Current state | Behavior |
|---|---|
| Created | Starts the initial connection |
| Disconnected | Starts a manual reconnection |
| Connecting | Waits for the connect attempt already in progress |
| Connected | Completes successfully at once, since the connection exists |
| Reconnecting | Waits for the result of the automatic reconnection in progress |
| Closed | Fails with an error |

## 3. Automatic Reconnection

Automatic reconnection is on by default. When the connection drops, the state becomes reconnecting
and the connector retries with the configured delays and attempt count. The delays and the way to
say unlimited are covered by [Connector Options](03-connector-options.en.md).

A send made while reconnection is in progress is not queued; it **fails as no connection.** Holding
the value and sending it after the connection returns is the application's decision — the connector
cannot know how long a value stays meaningful.

**When the connection drops, every pending request fails.** None is retransmitted after a successful
reconnect. Whether the same request should be sent again depends on what that request means.

## 4. Receiving State Changes and Drops

The connection-state handler runs on every state change. The disconnect handler runs **once when the
transport drops and once when the reconnect attempts are used up.** The first says there is no
connection right now; the second says the connector has given up restoring it. A successful
reconnect means the second never runs.

=== "C++"

    ```cpp
    auto disconnected = connector.on_disconnected (
      [] (std::optional<sc::close_reason_t> reason) { show_reconnecting (); });

    auto changed = connector.on_connection_state_changed (
      [] (const sc::connection_state_changed_t &event) {
          if (event.current == sc::connection_state_t::connected) {
              resubscribe ();
          }
      });
    ```

=== "C#/.NET"

    ```csharp
    using var disconnected = connector.OnDisconnected((closed, cancellationToken) =>
    {
        ShowReconnecting(closed.CloseReason);
        return ValueTask.CompletedTask;
    });

    using var changed = connector.OnConnectionStateChanged((change, cancellationToken) =>
    {
        if (change.Current == ZlinkStreamConnectionState.Connected) Resubscribe();
        return ValueTask.CompletedTask;
    });
    ```

=== "Java"

    ```java
    AutoCloseable disconnected = connector.onDisconnected(closed -> {
        showReconnecting(closed.closeReason());
        return CompletableFuture.completedFuture(null);
    });

    AutoCloseable changed = connector.onConnectionStateChanged(state -> {
        if (state == ZLinkStreamConnectionState.CONNECTED) {
            resubscribe();
        }
        return CompletableFuture.completedFuture(null);
    });
    ```

=== "Kotlin"

    ```kotlin
    val disconnected = connector.onDisconnected { closed ->
        showReconnecting(closed.closeReason())
        CompletableFuture.completedFuture(null)
    }

    val changed = connector.onConnectionStateChanged { state ->
        if (state == ZLinkStreamConnectionState.CONNECTED) {
            resubscribe()
        }
        CompletableFuture.completedFuture(null)
    }
    ```

=== "Node/TypeScript"

    ```typescript
    // The disconnect handler here takes no reason argument, so it is read from the property.
    const disconnected = connector.onDisconnected(() => {
      showReconnecting(connector.closeReason);
    });

    const changed = connector.onConnectionStateChanged(change => {
      if (change.current === ZlinkStreamConnectionState.Connected) {
        resubscribe();
      }
    });
    ```

**Whether the disconnect handler takes the reason as an argument is decided by the language.** Where
it does not, one read surface reports the reason. Either way there is a path to it.

When the connection is established again, the received counts return to zero and the unconsumed
messages of the previous connection are cleared. A subscription that has to be sent again right
after connecting belongs in the state handler.

## 5. Heartbeat

With the heartbeat on, the connector sends a control packet on the configured interval. If no frame
arrives for the configured timeout, it treats the transport as dropped and applies the reconnect
policy. That control packet is never delivered to a handler and never enters the receive queue.

Turning the heartbeat off still leaves an inbound ping answered.

## 6. Close Reasons

When a connection ends, the reason remains readable. The set of values is closed.

| Reason | Meaning |
|---|---|
| Client close | The client closed the connection |
| Idle timeout | The server closed an idle session |
| Heartbeat timeout | The heartbeat went unanswered and the connection dropped |
| Server drain | The server closed the session in a graceful drain |
| Protocol error | The connection dropped on a protocol violation |
| Transport error | The connection dropped on a transport-level failure |

After a graceful drain the client uses this value to decide when to reconnect and with what backoff.
The server does not name a replacement endpoint, so where to reconnect is decided by the client's own
configuration.

=== "C++"

    ```cpp
    auto reason = connector.close_reason ();   // Empty until the connection has ended once.
    ```

=== "C#/.NET"

    ```csharp
    var reason = connector.CloseReason;   // null until the connection has ended once.
    ```

=== "Java"

    ```java
    Optional<ZLinkStreamCloseReason> reason = connector.closeReason();
    ```

=== "Kotlin"

    ```kotlin
    // The coroutine wrapper has no read surface for this, so it is read from the Java connector.
    val reason: Optional<ZLinkStreamCloseReason> = javaConnector.closeReason()
    ```

=== "Node/TypeScript"

    ```typescript
    const reason = connector.closeReason;   // undefined until the connection has ended once.
    ```

**The reason can be read at any time.** Code that registered no disconnect handler reads the same
value after the connection closes. A first connect that failed also leaves a reason, and
reconnecting does not clear it: the value keeps the reason of the last ending.

## 7. What Closing Waits For

The close call waits for **the connector's own work only**: closing the transport and failing the
operations that were waiting. It does not write frames that have not gone out, and it does not wait
for the peer to read or respond. A close called outside a handler returns once that work is done. A
close called inside a handler returns right after starting it, so a handler never waits for the
close of the path that runs it.

**The connector does not wait for a handler to finish.** The connection state and disconnect
handlers that result from closing follow the dispatch mode: in `Immediate` the close work runs them,
and in `Manual` they run at the next dispatch pump after the close. Either way the close does not
observe whether they completed, and reconnection behaves the same way. That is why one handler that
never finishes cannot block the close. Work inside a handler that must be finished is awaited
outside the handler.

## 8. Next Chapters

- Error codes delivered with a drop — [Error Handling](07-error-handling.en.md)
- Reconnect delays and heartbeat settings — [Connector Options](03-connector-options.en.md)
