# 06 — Connection Lifecycle

[← Receiving Packets](05-receiving.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Error Handling →](07-error-handling.en.md)

---

## State Transitions

```
created → connecting → connected → disconnected
                    ↗                     |
              reconnecting ←————————————— |
                                          ↓
                                        closed
```

| State | Meaning |
|------|------|
| `created` | The connector was created, but `connect()` hasn't been called |
| `connecting` | Attempting a connection after calling `connect()` |
| `connected` | The connection is open, and packets can be sent/received |
| `reconnecting` | The connection dropped and a reconnect is being attempted |
| `disconnected` | The connection dropped, and reconnect is disabled or every attempt has been exhausted |
| `closed` | The connector was shut down by calling `close()` |

Read the current state with `connector.state()`.

## connect

```cpp
auto connected = connector.connect();
if (!connected) {
    // error_code_t::connect_timeout, error_code_t::disconnected, etc.
}
```

`connect()` blocks until the connection completes or fails. A callback style can also be used.

```cpp
connector.connect([](zlink::stream_connector::result_t<void> result) {
    if (!result) { return; }
    // packet sending can start after the connection succeeds
});
```

The callback `connect()` returns immediately after registering — it doesn't wait for the connection.

## close

```cpp
auto closed = connector.close();
```

`close()` cleans up pending requests, pending waits, and the received queue, then closes the
connection. Pending `request().submit(callback)` callbacks complete with a `closed` error. A
callback style can also be used.

```cpp
connector.close([](zlink::stream_connector::result_t<void> result) {
    // close completed
});
```

## reconnect

If `reconnect.enabled = true` (the default), a reconnect is automatically attempted after a
connection drop.

```cpp
options.reconnect.enabled        = true;
options.reconnect.initial_delay  = std::chrono::milliseconds{250};
options.reconnect.max_delay      = std::chrono::seconds{5};
options.reconnect.backoff_factor = 2.0;
options.reconnect.max_attempts   = 3;
```

The state changes to `reconnecting` while reconnecting. If every attempt fails, it transitions to
`disconnected`.

Calling `send()`, `request()` in the `reconnecting` state returns a `disconnected` error. After the
reconnect succeeds, packet sending/receiving is possible again.

## heartbeat

Heartbeat periodically confirms the connection is alive.

```cpp
options.heartbeat.enabled  = true;
options.heartbeat.interval = std::chrono::seconds{10};  // the ping interval while idle
options.heartbeat.timeout  = std::chrono::seconds{30};  // disconnected if no response within this time
```

- If there's no inbound traffic for `interval`, a `$zlink.heartbeat.ping` control frame is sent.
- If there's no inbound traffic for `timeout`, the connection is treated as `disconnected`.
- Heartbeat control frames aren't delivered to an `on<packet_t>()` callback.

## Receiving Status Events

```cpp
using zsc = zlink::stream_connector;

connector.on_connection_state_changed([](const zsc::connection_state_changed_t& ev) {
    switch (ev.state) {
    case zsc::connection_state_t::connected:
        // connection succeeded, send a join packet to the server
        break;
    case zsc::connection_state_t::reconnecting:
        // show "reconnecting..." in the UI
        break;
    case zsc::connection_state_t::disconnected:
        // reconnect failed, return to the lobby
        break;
    default:
        break;
    }
});

connector.on_disconnected([]() {
    // right after detecting a transport drop (before a reconnect attempt)
});
```

## is_connected()

Quickly checks the connection state before sending a packet.

```cpp
if (!connector.is_connected()) {
    // don't attempt to send
    return;
}
```

Even if `is_connected()` returns `true`, the connection can drop immediately afterward. Always check
the send/request return value.

## Lifecycle Order Example

```cpp
// 1. Create
auto connector = zsc::connector_factory_t::create(options);

// 2. Register events (register before connect to receive the initial connected event)
connector.on_connection_state_changed(state_handler);
connector.on<server_event_t>(event_handler);

// 3. Connect
auto result = connector.connect();

// 4. Use (game loop)
while (running) {
    connector.dispatch();
}

// 5. Shut down
connector.close();
```
