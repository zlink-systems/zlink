---
title: "9. STREAM · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/09-stream.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 8. Session And Actor Binding](08-actor-session.en.md) | [Next: 10. Location — Auto-Connect And Object Location](10-location.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/09-stream.en.md) · **C++** · [Java](../../../java/guide/server/09-stream.en.md) · [Kotlin](../../../kotlin/guide/server/09-stream.en.md) · [Node/TypeScript](../../../node/guide/server/09-stream.en.md)
<!-- language-switch:end -->

# 9. STREAM

> **The documents that own this chapter's contract** —
> [STREAM server session](../../../common/spec/19-stream-session.ko.md) owns the behavior,
> and the
> [per-language STREAM session public contract](../../../common/spec/server/languages/README.ko.md)
> owns the server's exact signatures. The client package follows the Stream Connector guide
> and the [per-language public contract](../../../common/spec/stream-connector/README.en.md).

STREAM is a connection-oriented, bidirectional message channel between an external client and
the Framework server. The server implements session lifecycle and packet dispatch. The
client uses the independent package `Systems.Zlink.Stream.Connector`.

## 1. Registering A Server Node

Register one session type on a Stream node. If you use Actor dispatch, enable it explicitly.

```cpp
options.add_stream_node ("client-stream")
  .bind ("tcp://0.0.0.0:9100")
  .enable_actor_dispatch ()
  .register_session<play_session_t> (); // Registers the session type to create per connection.
```

A session handler and Actor/Spot handler use the Framework's default typed JSON
serialization. The application doesn't register a codec per message type or parse a raw
frame itself.

**Registration is explicit.** There's no surface that implicitly registers a stream node via
an attribute, annotation, or decorator. There are only three axes — node name, bind
endpoint, session type. Of these, **the bind endpoint must always be specified.**

The following eight are blocked as configuration errors **before host startup**, not
deferred to the first connection.

| Condition |
| --- |
| The node name is empty |
| The same node name is registered twice |
| There's no bind endpoint |
| The same session type is registered more than once |
| More than one session is registered on one node |
| TLS is on but the certificate path is empty |
| TLS is on but the key path is empty |
| A client certificate is required without a TLS server configured |

If you enable TLS, specify both the certificate and key paths together. Requiring a client
certificate is off by default; turning it on rejects a connection that fails verification
**before a session is ever created.**

## 2. Session Lifecycle

A session implements the connection, packet dispatch, and error/disconnect callbacks. A
given session's callbacks run serially.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the session that represents one client connection. It filters out the authenticate
> packet first and relays everything else to the Actor. Actual code from the repository.

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp:doc-session"
```

In its minimal shape, it looks like this.

```cpp
// A C++ session inherits packet_stream_session_t and overrides the callbacks.
class play_session_t : public packet_stream_session_t
{
  public:
    task_t<void> on_connected (stream_t &stream) override
    {
        _logger.info ("connected");
        co_return;
    }

    task_t<void> on_packet (stream_t &stream,
                            const stream_dispatch_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        if (!_ping.can_handle (dispatch)) {
            // Closes the connection on a packet outside the application protocol.
            co_await stream.close ();
            co_return;
        }
        co_await _ping.handle (stream, payload);
    }

    task_t<void> on_error (stream_t &, const stream_error_t &error) override
    {
        _logger.warn (std::string ("session error: ") + error.message);
        co_return;
    }

    task_t<void> on_disconnected (stream_t &) override
    {
        _logger.info ("disconnected");
        co_return;
    }
};
```

Where an error goes splits four ways. **The session error callback receives only a
transport error that belongs to that session.**

| Error | Where it goes |
| --- | --- |
| That session's transport error | The session error callback |
| A handshake failure | Runtime monitoring. There's no target to call — the session hasn't been created yet |
| A socket/node-level error | Runtime monitoring. Can't be pinned to one session's error |
| An application handler exception | The handler exception path. **Not the session error callback** |

**A handler filter doesn't apply to session dispatch.** Even a filter attached to a
different dispatch never runs ahead of a session callback. Anything that needs filtering on
the session path, like authentication, is handled through the session's own handler
registration.

**There's no surface that runs a recv loop directly.** The Framework enqueues the packet and
then runs the session callback, applying dispatch, DI, and logging consistently at that
boundary. This is by design, so the application never has to carry the loop, cancellation,
or backpressure itself.

## 3. Typed Packet Handler

The handler registry decodes the received message into a typed message. When replying to a
request, use the current dispatch's one-shot reply token.

```cpp
// A C++ typed handler receives only the stream and the decoded payload. The dispatch
// context is passed only to the raw on_packet path, so can_reply isn't checked here.
task_t<void> handle (stream_t &stream, const ping_t &message)
{
    // Replies exactly once, using the same request correlation. Ends in failure if it isn't a request.
    co_await stream.reply_packet (zlink::message_t::from_json (pong_t{message.sequence}))
      .submit ();
}
```

`reply` is valid only for the current request and can be submitted once. Even if the send
fails on a timeout or cancellation, the same reply token can't be reused.

**No packet name rides on the response.** The client finds the pending request purely by
request sequence, and **the type specified at the call site** decides what type to read the
response as. Because it's not selected by name, there's no surface to attach a packet name
to the response side either. An error response also comes back on the same sequence.

Use `send` when the server pushes first.

```cpp
// Waits for admission into the local transport queue.
co_await stream.send (server_notice_t{"maintenance"})
  .metadata ("severity", "info")
  .compress ()
  .submit ();
```

## 4. Actor Dispatch

After authentication, bind an Actor to the session, and a message not handled by a
session-only handler can be handed off through the session actor's relay call. The detailed
flow follows [Session And Actor Binding](08-actor-session.ko.md).

The application doesn't query the session route from the Location Store directly. Once Actor
relocation completes, the Framework updates the binding route.

## 5. Client Connection

The client uses the Stream Connector package, not the server Framework package.

```cpp
zlink::stream_connector::connector_options_t connector_options;
connector_options.endpoint = "tcp://game.example.com:9100";
connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
auto connector = zlink::stream_connector::connector_factory_t::create (connector_options);

connector.on<game_state_notify_t> ("GameStateNotify",
                                   [] (const auto &message) { render (message.payload ()); });

co_await connector.connect ().submit (); // Finishes connecting and preparing the receive loop.

while (running) {
    co_await connector.dispatch ().submit (); // manual mode runs the callback on this caller.
}
```

Use `Manual` when the callback needs to run on a game loop or UI thread. `Immediate` runs
the callback on the connector's own worker, so it doesn't fit a client that needs thread
affinity.

## 6. Client Send And Request

```cpp
// Waits for admission into the bounded outbound queue.
co_await connector.send (player_input_t{direction}).submit ();

// Finds the response by request sequence.
auto profile = co_await connector.request (get_profile_t{player_id}).submit<profile_t> ();
```

The Connector's default typed codec is JSON. Packet name override, push waiting, reconnect,
heartbeat, and bounded queue settings are explained in the Stream Connector guide.

## 7. Related Documents

- Runnable verification examples for this chapter's contract: `13. Interface Catalog`
  chapter §5 — the verification class `StreamContracts`
- Session and Actor binding: [Session Actor Dispatch](08-actor-session.ko.md)
- Full client connector usage: the Stream Connector guide
- Location Store and auto-connect: [Location](10-location.ko.md)
