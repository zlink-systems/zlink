# 05 — Receiving Packets

[← Sending Packets](04-sending.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Connection Lifecycle →](06-lifecycle.en.md)

---

## on\<T\>() — Registering A Push Callback

Receives a push packet the server sends, via a callback.

```cpp
struct leaderboard_update_t {
    int32_t rank;
    std::string player_id;
    int64_t score;
};

connector.on<leaderboard_update_t>([](const leaderboard_update_t& update) {
    // update.rank, update.player_id, update.score
});
```

The packet name can be specified explicitly.

```cpp
connector.on<leaderboard_update_t>(
    "leaderboard.weekly",
    [](const leaderboard_update_t& update) {
        // process only the weekly leaderboard
    });
```

`on()` adds to a callback list owned by the connector. When the connector closes, the callbacks are
also removed. Currently, there's no API to individually unregister an already-registered callback.

## Dispatch Mode

`dispatch_mode` decides when an `on<T>()` callback runs.

### Manual Mode (Default)

The callback runs on the thread that called `dispatch()`. Use this to align with a game engine's
frame loop.

```cpp
// connector_options_t::dispatch_mode = dispatch_mode_t::manual (default)

// game loop
while (running) {
    connector.dispatch(); // runs pending push-packet callbacks
    update_game_state();
    render();
}
```

`dispatch()` processes whatever callbacks are pending at call time and returns. It doesn't wait for
a newly arriving packet.

### Immediate Mode

The callback runs immediately on the connector's receive path. No call to `dispatch()` is needed.
Suits a CLI, tool, or e2e client.

```cpp
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
```

Even in immediate mode, the callback runs outside the connector's internal lock. It's safe to call
`connector.send()` inside the callback.

## wait_for\<T\>() — Waiting For A Specific Packet

Waits once for a specific packet, with no callback registered. A matched packet is consumed, and is
not delivered to an `on<T>()` callback afterward.

```cpp
struct server_ready_t {
    std::string server_id;
    int32_t player_capacity;
};

auto ready = connector
    .wait_for<server_ready_t>()
    .packet_name("server.ready")
    .timeout(std::chrono::seconds{10})
    .submit();

if (!ready) {
    // ready.error_code() == error_code_t::request_timeout, etc.
    return;
}

auto capacity = ready.value().player_capacity;
```

If timeout is omitted, `connector_options_t::wait_timeout` is used.

### where() — Conditional Filter

Consumes only a packet matching a specific condition. A packet that doesn't match isn't consumed —
it stays in the queue, to be processed by a later wait or dispatch.

```cpp
auto my_match = connector
    .wait_for<match_found_t>()
    .where([](const match_found_t& msg) {
        return msg.match_id == "match-7f3a";
    })
    .timeout(std::chrono::seconds{30})
    .submit();
```

If you only need to check whether a single field equals a specific value, you can use the
member-pointer overload. This avoids repeating a C++ lambda's parameter declaration.

```cpp
auto my_match = connector
    .wait_for<match_found_t>()
    .where(&match_found_t::match_id, std::string("match-7f3a"))
    .timeout(std::chrono::seconds{30})
    .submit();
```

### The wait_for Callback Style

```cpp
connector
    .wait_for<server_ready_t>()
    .packet_name("server.ready")
    .submit([](zlink::stream_connector::result_t<server_ready_t> result) {
        if (!result) { return; }
        // result.value()
    });
```

## Connection State Events

```cpp
connector.on_connection_state_changed([](const zlink::stream_connector::connection_state_changed_t& ev) {
    // ev.state: created, connecting, connected, reconnecting, disconnected, closed
});

connector.on_disconnected([]() {
    // the connection dropped (before a reconnect attempt)
});

connector.on_error([](const zlink::stream_connector::error_t& err) {
    // err.code, err.message
});
```

## Calling send/request Inside A Callback

You can call the same connector's `send()`, `request()` while a callback is running. The
implementation doesn't run the user callback while holding the connector's internal lock.

```cpp
connector.on<pvp_invite_t>([&connector](const pvp_invite_t& invite) {
    // safe to send inside the callback
    connector
        .send(pvp_accept_t{invite.match_id, "player-1"})
        .packet_name("pvp.accept")
        .submit();
});
```

## pending_dispatch_count()

Checks the number of pending callbacks in manual mode.

```cpp
auto pending = connector.pending_dispatch_count();
```
