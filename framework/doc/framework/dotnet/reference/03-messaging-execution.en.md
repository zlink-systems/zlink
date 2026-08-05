# 03. Messaging execution — Channel messaging

[Reference index](README.en.md)

This category covers the entry points `IZLinkRouteClient` and `IZLinkFanoutClient` provide. The
exact signatures are owned by the
[Channel messaging exact interface](../../common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md)
(Korean-only). This document does not repeat that signature — it collects only what is needed to
complete a call to each entry point in practice.

---

## `SendToChannel<TMessage>`

Sends a one-way message to one ready target (RouteMesh or ClientServer) registered under a
ChannelName. It does not wait for a reply.

```csharp
await routeClient
    .SendToChannel("game.api", new PlayerOnline("player-1"))
    .Async(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Metadata(key, value)` / `.Metadata(ZLinkMessageMetadata)` | none | key-value passed to the handler. Bounded to 1024 bytes (UTF-8) combined; the last value wins for a repeated key |
| `.Async(ct)` | required terminal | waits only until source-local admission succeeds |

**Completion.** Normal completion means this process accepted the message onto its queue. It
does not wait for remote handler execution or subscriber receipt. If there is no queue room, it
waits until the socket send timeout (1 second if unset, changed with
`SetDefaultSocketSendTimeout`) and then completes with `DeadlineExceeded`. If the ChannelName has
no ready target it completes with `NotFound`; a broken route completes with `Unavailable`; a
runtime shutting down completes with `ShuttingDown`.

**When to use it.** Use it for fire-and-forget where no reply is needed. If a reply is needed,
use `RequestToChannel`.

---

## `RequestToChannel<TRequest, TResponse>`

Selects one ready target under a ChannelName, sends a typed request, and waits for a typed
reply.

```csharp
var reply = await routeClient
    .RequestToChannel("game.api", new GetPlayer("player-1"))
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<Player>(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Metadata(key, value)` / `.Metadata(ZLinkMessageMetadata)` | none | attaches only to the request; a reply does not automatically copy the request's metadata |
| `.Timeout(TimeSpan)` | `DefaultRequestTimeout` (30 seconds by default, changed with `SetDefaultRequestTimeout`) | the upper bound for waiting on the reply. The socket send timeout separately covers transport admission |
| `.Async<TResponse>(ct)` | terminal (choose one) | keeps the current execution waiting until the reply arrives |
| `.Yield<TResponse>(ct)` | terminal (choose one) | valid only inside a `SpotWide` User Spot·Instance Spot handler. While waiting it releases the User Spot gate to let a sibling job proceed. Calling it in any other execution context completes with `InvalidOperation` |

**Completion.** Completes with `TResponse` (the handler's return value), or with
`DeadlineExceeded` on timeout, `NotFound` if the ChannelName has no ready target, `Unavailable`
on a broken route, or `ShuttingDown` while the runtime is shutting down.

**When to use it.** Use it when a reply value is needed. For one-way, use `SendToChannel`. Use
`Yield` inside a `SpotWide` handler so that your own wait does not block a sibling job while
another request or worker is in progress.

---

## `SendToNode<TMessage>`

Sends a one-way message by directly specifying a MeshName and a target Node RID. Use it only to
manage one specific MeshNode, not for ChannelName-based selection.

```csharp
await routeClient
    .SendToNode("play", RoutingId.From("play-node-1"), new DrainRequested())
    .Async(ct);
```

**Options.** Same as `SendToChannel` — `.Metadata(...)`, terminal `.Async(ct)`.

**Completion.** Uses the same completion kinds as `SendToChannel`. If the target RID is an
Object Client (an RID that cannot register a handler), it completes with `NotFound` instead of
forwarding to another target.

**When to use it.** Do not use it for placing or messaging business objects (actor·spot) — use
ActorId·SpotId·ChannelName for that. Use Node direct only to target one specific node for
operational purposes.

---

## `RequestToNode<TRequest, TResponse>`

Directly specifies a MeshName and a target Node RID to exchange a typed request/reply.

```csharp
var status = await routeClient
    .RequestToNode("play", RoutingId.From("play-node-1"), new GetNodeStatus())
    .Async<NodeStatus>(ct);
```

**Options.** Same as `RequestToChannel` — `.Metadata(...)`, `.Timeout(...)`, terminal
`.Async<TResponse>(ct)` or `.Yield<TResponse>(ct)`.

**Completion and usage.** Same as `RequestToChannel`, except that target selection is a fixed
specified RID rather than ChannelName round-robin.

---

## `Publish<TEvent>` (classic fanout)

Publishes a typed event to an independent fanout channel. This is a different family from
`IZLinkRouteClient`'s channel operations — the publisher does not know its subscribers.

```csharp
await fanoutClient
    .Publish("lobby.events", new PlayerJoined("player-1"))
    .Async(ct);

// when an explicit topic is required
await fanoutClient
    .Publish("lobby.events", "region.eu", new PlayerJoined("player-1"))
    .Async(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| omitting the topic argument | uses the event's packet name as the topic | using a reserved topic name completes with `ArgumentException` |
| `.Async(ct)` | required terminal | waits only until source-local publish admission completes |

**Completion.** Normal completion means publish admission finished. It does not report
subscriber count or receipt — it completes normally even with zero targets. Once started, it
does not turn an individual target's failure into an overall failure, and it does not retry.

**When to use it.** Use it for observation·notification where the publisher must not know its
subscribers. For messaging aimed at a specific target, use `SendToChannel` or
`RequestToChannel`.

---

## Codec registration (configuration time)

Unlike the other entries, this is a host configuration-time registration call, not a terminal
await. An application that only uses JSON does not need this entry.

```csharp
services.AddZLinkFramework(options =>
{
    options.Codecs.Use(ZLinkMessagePackCodec.Default);
});
```

**Options.** The following modifier attaches to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Use(IZLinkCodecExtension)` | JSON if unset | registers a business payload serializer. Call it multiple times to register multiple content types |

**Completion.** Registers synchronously with no return value. Call it only before the host
starts — a call after startup is outside the contract.

**When to use it.** Use it when the content type is not JSON (MessagePack, Protobuf, etc.).
Besides the official `ZLinkMessagePackCodec.Default`/`ZLinkProtobufCodec.Default`, a custom
serializer can also be registered by implementing `IZLinkCodecExtension` directly.

The wire codec for a STREAM connection is a separate contract
(`IZlinkStreamCodecRegistration`, owned by the Stream Connector). This entry covers only the
business payload serializer.

---

## Common failure·cancellation rules (apply to every entry)

These apply in common to every entry point in this category and are not repeated per entry.

- If the `CancellationToken` is already triggered before admission, it completes as cancelled
  exactly once and does not start admission.
- When admission, timeout, shutdown, and cancellation race, exactly one becomes the terminal
  atomically, and no late admission is created afterward.
- An invalid argument, handle, or state is handled as a .NET exceptional completion
  (an exception) — a different layer from the completion kinds this document lists
  (`NotFound`/`Unavailable`/`DeadlineExceeded`/`ShuttingDown`).

The full basis is the
[Channel messaging exact interface](../../common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md) and the
[Common runtime exact interface](../../common/spec/server/languages/dotnet/interfaces/01-common-runtime.ko.md)
(both Korean-only).
