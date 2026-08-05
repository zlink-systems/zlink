# 03. Messaging execution — Channel messaging

[Reference index](README.en.md)

This category covers the entry points `route_client_t` and `publisher_t` provide. The exact
signatures are owned by the
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md)
(Korean-only). This document does not repeat those signatures — it collects only what you need to
actually call each entry point, in complete form.

The Framework registers several typed clients with the same meaning via DI by default
(`request_client_t`, `message_bus_t` also provide ChannelName-based send/request). This document
is written around `route_client_t`, the closest match to .NET's `IZLinkRouteClient`; the other
clients are alternate surfaces that share the same completion kinds and admission rules.

---

## `send_to_channel<TMessage>`

Sends a one-way message to one ready target (RouteMesh or ClientServer) registered under a
ChannelName. Does not wait for a reply.

```cpp
co_await route_client
  .send_to_channel("game.api", player_online_t{"player-1"})
  .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the handler |
| `.submit()` | Required terminal | Waits only until source-local admission succeeds |

**Completion result.** A normal completion means this process accepted the message into the
queue. It does not wait for remote handler execution or subscriber reception. If the queue has no
room, it waits until the socket send timeout (1 second if not configured) and then completes with
`deadline_exceeded` if it still has none. No ready target for the ChannelName completes with
`not_found`, a route disconnect with `unavailable`, and a runtime shutting down with
`shutting_down`, as a `framework_exception_t`.

**When to use.** Use this for fire-and-forget where no reply is needed. Use `request_to_channel`
if a reply is needed.

---

## `request_to_channel<TRequest>`

Selects one ready target by ChannelName, sends a typed request, and waits for a typed reply.

```cpp
player_t reply = co_await route_client
  .request_to_channel("game.api", get_player_t{"player-1"})
  .timeout(std::chrono::seconds{3})
  .submit<player_t>();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Attaches only to the request. The reply does not automatically copy request metadata |
| `.timeout(milliseconds)` | The MeshNode's `set_default_request_timeout(...)` value | The upper bound for waiting on the reply. Send admission itself is handled separately by the socket send timeout |
| `.submit<TReply>()` | terminal (pick one) | Keeps the current coroutine waiting until the reply arrives |
| `.yield<TReply>()` | terminal (pick one) | Only valid inside a `spot_wide` User Spot/Instance Spot handler. Returns the shared turn while waiting, allowing sibling jobs to run. Calling it from any other execution context completes with `invalid_operation` |

**Completion result.** Completes with `TReply` (the handler's return value), or with
`deadline_exceeded` on timeout, `not_found` if the ChannelName has no ready target, `unavailable`
on a route disconnect, or `shutting_down` while the runtime is shutting down, as a
`framework_exception_t`.

**When to use.** Use this when the reply value is needed. Use `send_to_channel` if it is one-way.
Use `yield` so that, inside a `spot_wide` handler, waiting for this call does not block a sibling
job while another request or worker is in progress.

---

## `send_to_node<TMessage>`

Sends a one-way message by specifying the MeshName and target Node RID directly. Used to manage a
specific MeshNode rather than a ChannelName-based selection.

```cpp
co_await route_client
  .send_to_node("play", zlink::routing_id_t::from("play-node-1"),
                drain_requested_t{})
  .submit();
```

**Options.** The same as `send_to_channel` — `.metadata(...)`, terminal `.submit()`.

**Completion result.** Uses the same completion kinds as `send_to_channel`. If the target RID is
an Object Client (an RID that cannot register handlers), it completes with `not_found` without
handing off to another target.

**When to use.** Do not use this for business object (actor/spot) placement or messaging — use
ActorId/SpotId/ChannelName for that. Use Node direct only to target a specific node for
operational purposes.

---

## `request_to_node<TRequest>`

Sends and receives a typed request/reply by specifying the MeshName and target Node RID directly.

```cpp
node_status_t status = co_await route_client
  .request_to_node("play", zlink::routing_id_t::from("play-node-1"),
                    get_node_status_t{})
  .submit<node_status_t>();
```

**Options.** The same as `request_to_channel` — `.metadata(...)`, `.timeout(...)`, terminal
`.submit<TReply>()` or `.yield<TReply>()`.

**Completion result and when to use.** Same as `request_to_channel`, except target selection is
fixed to the specified RID rather than ChannelName round-robin.

---

## `publish<TEvent>` (classic fanout)

Publishes a typed event to an independent fanout channel. A different family from
`route_client_t`'s channel operations — the publisher does not know its subscribers.

```cpp
co_await publisher
  .publish("lobby.events", player_joined_t{"player-1"})
  .submit();

// when a topic must be specified explicitly
co_await publisher
  .publish("lobby.events", "region.eu", player_joined_t{"player-1"})
  .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| Omitting the topic argument | Uses the event's `packet_name` as the topic | Using the reserved topic name (the internal liveness exact bytes `01 5A 4C 46 31`) completes with `framework_exception_t` |
| `.submit()` | Required terminal | Waits only until source-local publish admission completes |

**Completion result.** A normal completion means publish admission finished. It does not return
the subscriber count or reception completion — completing normally even with 0 targets. Once
started, an individual target failure does not turn into an overall failure and is not retried.

**When to use.** Use this for observation/notification where the publisher must not know its
subscribers. For messaging aimed at a specific target, use `send_to_channel` or
`request_to_channel`.

---

## Codec registration (configuration time)

Unlike other entries, this is a registration call made at host configuration time, not a
terminal. Applications that only use JSON do not need this entry.

```cpp
app.add_zlink_framework([&](auto &options) {
    options.codecs().use(protobuf_codec_extension); // the value the chosen codec extension package provides
});
```

```cmake
# add this only when Protobuf is needed.
target_link_libraries(app PRIVATE zlink::framework_codec_protobuf)
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.use(extension)` | JSON if omitted | Registers a business payload serializer. Can be called multiple times to register several content types |

**Completion result.** Registers synchronously with no return value. Call this only before the
host starts.

**When to use.** `options.codecs().use(...)` is not a step for enumerating ordinary message
types — it is an advanced extension point for connecting a payload that cannot be expressed with
the default JSON, or a separate binary serializer extension. Handler and client typed APIs read
`request_type`/`reply_type`/`message_type`/`event_type` from handler registration and pick the
JSON serializer automatically, so you do not need to list every request/reply type in the codec
configuration.

A STREAM connection's wire codec is a separate contract
(`stream_compression_options_builder_t`, the "Other host-wide options" entry in the
topology-discovery category). This entry only covers business payload serializers.

---

## Common failure/cancellation rules (apply to every entry)

These apply in common to every entry point in this category and are not repeated per entry.

- A C++ server call has no separate cancellation argument. Not holding onto, or destroying, the
  returned `task_t` does not guarantee the operation is cancelled.
- When admission, timeout, and shutdown race, exactly one becomes terminal atomically, and no
  late admission is created afterward.
- Invalid arguments/handles/state, and a duplicate `submit()`, complete with
  `framework_exception_t` — the same exception type as the completion kinds this document lists
  (`not_found`/`unavailable`/`deadline_exceeded`/`shutting_down`), but distinguished by `kind()`.

See the
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md)
and the
[Common runtime exact interface](../../common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md)
(Korean-only) for the full rationale.
