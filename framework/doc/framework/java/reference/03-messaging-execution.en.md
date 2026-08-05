# 03. Messaging execution — Channel messaging

[Reference index](README.en.md)

This category covers the entry points `ZLinkRouteClient` and `ZLinkFanoutClient` provide. The
exact signatures are owned by the
[Java channel messaging exact interface](../../common/spec/server/languages/java/interfaces/channel-messaging.en.md)
(Korean-only). This document does not repeat those signatures — it collects only what you need to
actually call each entry point, in complete form.

The Framework also provides `ZLinkClient` (ChannelName-only, `sendToChannel`/`requestToChannel`
only) with the same meaning via DI. This document is written around `ZLinkRouteClient`, the
closest match to .NET's `IZLinkRouteClient`.

---

## `sendToChannel`

Sends a one-way message to one ready target (RouteMesh or ClientServer) registered under a
ChannelName. Does not wait for a reply.

```java
routeClient.sendToChannel("game.api", new PlayerOnline("player-1"))
    .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` / `.metadata(Map<String, String>)` | None | Key-value to pass to the handler |
| `.submit()` | Required terminal | Waits only until source-local admission succeeds. Returns `CompletionStage<Void>` |

**Completion result.** A normal completion means this process accepted the message into the
queue. It does not wait for remote handler execution or subscriber reception. If the queue has no
room, it waits until the socket send timeout and then completes with `DEADLINE_EXCEEDED` if it
still has none. No ready target for the ChannelName completes with `NOT_FOUND`, a route
disconnect with `UNAVAILABLE`, and a runtime shutting down with `SHUTTING_DOWN`, as a
`ZLinkFrameworkException`.

**When to use.** Use this for fire-and-forget where no reply is needed. Use `requestToChannel` if
a reply is needed.

---

## `requestToChannel`

Selects one ready target by ChannelName, sends a typed request, and waits for a typed reply.

```java
CompletionStage<Player> reply = routeClient
    .requestToChannel("game.api", new GetPlayer("player-1"))
    .timeout(Duration.ofSeconds(3))
    .submit(Player.class);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` / `.metadata(Map<String, String>)` | None | Attaches only to the request. The reply does not automatically copy request metadata |
| `.timeout(Duration)` | The MeshNode's `setDefaultRequestTimeout(...)` value | The upper bound for waiting on the reply. Send admission itself is handled separately by the socket send timeout |
| `.submit(TReply.class)` | terminal (pick one) | Waits until the reply arrives |
| `.yield(TReply.class)` | terminal (pick one) | Only valid inside a `SpotWide` User Spot/Instance Spot handler. Returns the shared turn while waiting, allowing sibling jobs to run. Calling it from any other execution context completes with `INVALID_OPERATION` |

**Completion result.** Completes with `TReply` (the handler's return value), or with
`DEADLINE_EXCEEDED` on timeout, `NOT_FOUND` if the ChannelName has no ready target, `UNAVAILABLE`
on a route disconnect, or `SHUTTING_DOWN` while the runtime is shutting down, as a
`ZLinkFrameworkException`. A `ZLinkRequestFailureException` completes as one of
`TIMEOUT`/`CANCELLED`/`SHUTDOWN`.

**When to use.** Use this when the reply value is needed. Use `sendToChannel` if it is one-way.
Use `yield` so that, inside a `SpotWide` handler, waiting for this call does not block a sibling
job while another request or worker is in progress.

---

## `sendToNode`

Sends a one-way message by specifying the MeshName and target Node RID directly. Used to manage a
specific MeshNode rather than a ChannelName-based selection.

```java
routeClient.sendToNode("play", RoutingId.from("play-node-1"), new DrainRequested())
    .submit();
```

**Options.** The same as `sendToChannel` — `.metadata(...)`, terminal `.submit()`.

**Completion result.** Uses the same completion kinds as `sendToChannel`. If the target RID is an
Object Client (an RID that cannot register handlers), it completes with `NOT_FOUND` without
handing off to another target.

**When to use.** Do not use this for business object (actor/spot) placement or messaging — use
ActorId/SpotId/ChannelName for that. Use Node direct only to target a specific node for
operational purposes.

---

## `requestToNode`

Sends and receives a typed request/reply by specifying the MeshName and target Node RID directly.

```java
CompletionStage<NodeStatus> status = routeClient
    .requestToNode("play", RoutingId.from("play-node-1"), new GetNodeStatus())
    .submit(NodeStatus.class);
```

**Options.** The same as `requestToChannel` — `.metadata(...)`, `.timeout(...)`, terminal
`.submit(TReply.class)` or `.yield(TReply.class)`.

**Completion result and when to use.** Same as `requestToChannel`, except target selection is
fixed to the specified RID rather than ChannelName round-robin.

---

## `publish` (classic fanout)

Publishes a typed event to an independent fanout channel. A different family from
`ZLinkRouteClient`'s channel operations — the publisher does not know its subscribers.

```java
fanoutClient.publish("lobby.events", new PlayerJoined("player-1"))
    .submit();

// when a topic must be specified explicitly
fanoutClient.publish("lobby.events", "region.eu", new PlayerJoined("player-1"))
    .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| Omitting the topic argument | Uses the event's packet name as the topic | Using the reserved topic name (the internal liveness exact bytes `01 5A 4C 46 31`) completes with `ZLinkConfigurationException` |
| `.submit()` | Required terminal | Waits only until source-local publish admission completes |

**Completion result.** A normal completion means publish admission finished. It does not return
the subscriber count or reception completion — completing normally even with 0 targets. Once
started, an individual target failure does not turn into an overall failure and is not retried.

**When to use.** Use this for observation/notification where the publisher must not know its
subscribers. For messaging aimed at a specific target, use `sendToChannel` or `requestToChannel`.

---

## Codec registration (configuration time)

Unlike other entries, this is a registration call made at host configuration time, not a
terminal. Applications that only use JSON do not need this entry.

```java
options.codecs().use(ZLinkMessagePackCodec.defaultCodec());
```

```gradle
// add this only when MessagePack is needed.
implementation("systems.zlink:zlink-framework-codec-msgpack")
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.use(extension)` | JSON if omitted | Registers a business payload serializer. Can be called multiple times to register several content types |

**Completion result.** Registers synchronously with no return value. Call this only before the
host starts.

**When to use.** Use this when a non-JSON content type (MessagePack, Protobuf, etc.) is needed.
Besides the official `ZLinkMessagePackCodec.defaultCodec()`/`ZLinkProtobufCodec.defaultCodec()`,
you can also implement `ZLinkCodecExtension` directly to register a custom serializer.

A STREAM connection's wire codec is a separate contract (`ZLinkStreamCompressionBuilder`, the
"Other host-wide options" entry in the topology-discovery category). This entry only covers
business payload serializers.

---

## Common failure/cancellation rules (apply to every entry)

These apply in common to every entry point in this category and are not repeated per entry.

- When admission, timeout, and shutdown race, exactly one becomes terminal atomically, and no
  late admission is created afterward.
- Calling `toCompletableFuture().cancel(...)` on the returned `CompletionStage` releases only the
  waiter — it does not guarantee cancelling admission that has already started.
- Invalid arguments/handles/state complete with `ZLinkFrameworkException` (or
  `IllegalArgumentException`/`IllegalStateException`), and the completion kinds this document
  lists (`NOT_FOUND`/`UNAVAILABLE`/`DEADLINE_EXCEEDED`/`SHUTTING_DOWN`) are distinguished by
  `ZLinkFrameworkException.kind()`.

See the
[Java channel messaging exact interface](../../common/spec/server/languages/java/interfaces/channel-messaging.en.md)
and the
[Java common runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.en.md)
(Korean-only) for the full rationale.
