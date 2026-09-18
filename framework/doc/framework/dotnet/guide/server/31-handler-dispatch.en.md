---
title: "Handlers and Message Processing · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/31-handler-dispatch.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Handlers and Message Processing

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: How Channels Work](30-channel-patterns.en.md) | [Next: How STREAM Works](38-stream-boundary.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/31-handler-dispatch.en.md) · **C#/.NET** · [Java](../../../java/guide/server/31-handler-dispatch.en.md) · [Kotlin](../../../kotlin/guide/server/31-handler-dispatch.en.md) · [Node/TypeScript](../../../node/guide/server/31-handler-dispatch.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can work with what applies across many handlers at once — packet names,
    filters, and codecs. This chapter's code comes from the repository's
    tutorials and samples.

[Channel Messaging](20-channel-messaging.en.md) covered the path from writing one handler to
calling it. This chapter covers **what applies across many handlers**: the variations on
registration, the filter that collects shared processing, and the codec that turns a payload
into bytes.

This chapter's code comes from the repository's tutorials and samples.

## 1. Variations on Handler Registration

### 1.1 Several Channels on One MeshNode

You can register several channels on the same MeshNode, and **each channel can carry a
different role.** A channel that only receives registers with `Server()`; a channel that only
calls registers with `Client()`.

```csharp
--8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Program.cs:doc-multi-channel-register"
```

This node registers the `zone` channel with `Server()` and handles it, and registers the
`report` channel with `Client()` so it only sends. The two registrations sit side by side on
the same MeshNode and there is still only one socket.

### 1.2 The Order a Packet Name Is Decided In

A handler and a call find each other by **packet name**. The name is decided in this order.

1. The `packetName` argument passed at handler registration
2. The packet name marked on the payload type
3. The type name, when neither of the above is present

A packet name is settled **once, at registration**; there is no surface for naming it again per
call.

**The sending side and the receiving side have to arrive at the same name.** Give `packetName`
only at registration and the sending side falls through to 2 or 3 and uses the type name, the
two names diverge, and that call ends as if no handler existed. When there is no reason to pick
a name, leave it out on both sides and let 3 decide.

The same packet name may be reused under a different MeshName or ChannelName.

**Where you name it explicitly differs by language.** Some pass the name at the registration
call; others mark the name on the payload type. Those are the first and the second rule in the
order above.

```csharp
--8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs:doc-explicit-packet-name"
```

Do neither and the type name becomes the packet name as it stands. Every registration in the
tutorial works that way.

### 1.3 When a Packet Arrives with No Handler

| Call | Result |
| --- | --- |
| `request` | Fails with an error reply. The caller receives it as an exception |
| `send` | Dropped silently |

Dropped means the caller gets no reply, not that nothing is observable. The logger and telemetry
providers you configured receive the dispatch failure as a `no_handler`, `reply_error`, or
`drop` structured record ([Monitoring](26-monitoring.en.md)).

## 2. Filters — Collecting Shared Processing in One Place

The HTTP middleware of a web framework belongs to the HTTP pipeline, so it does not apply to
handlers. Work that would otherwise repeat across many handlers — logging, validation,
permission checks, measurement — goes into a filter.

<iframe class="zlink-diagram" src="/common/diagrams/31-filter-scope-en.html" title="A filter wraps the messages a node receives" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/31-filter-scope-en.html" target="_blank">↗ View larger</a></p>

What it wraps is the heart of this feature. A filter wraps **only the messages a node receives**,
so applying the same code to a Spot or Actor handler means placing it there separately. The
sections below take writing, registration order, and scope in turn.

### 2.1 Writing a Filter

Implement the filter interface and call `next`. The handler does not run unless `next` is
called. For the exact names, read your language's tab below.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Dispatch/CallLogFilter.cs:filter-implementation"
```

### 2.2 Registration Order Is Execution Order

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:filter-register"
```

Filters pass in front of the handler in registration order, and unwind in reverse order once
`next` returns.

```text
first filter, leading part
  -> second filter, leading part
       -> handler
     second filter, trailing part
first filter, trailing part
```

Each filter calls `next` **at most once**. Calling it twice does not run the handler again; it
is rejected as an error and classified as a coding mistake.

### 2.3 Where Filters Apply

A filter applies to **the messages a node receives**. It does not apply to a handler owned by an
object with a lifetime of its own, such as a Spot or an Actor.

| Dispatch | Filter |
| --- | --- |
| Channel send and request (both RouteMesh and ClientServer) | Runs |
| Fanout subscription handler | Runs |
| Node direct handler | Runs |
| Spot handler, Actor handler | Does not run |
| Logical Multicast subscription registered by a Spot | Does not run |
| STREAM session handler | Does not run |

To treat paths differently, read `context.DispatchKind`. `ChannelSend` and `ChannelRequest`
cover RouteMesh and ClientServer together, so when you need to tell those two apart, read
`context.MeshName` alongside it — RouteMesh and Node direct provide a MeshName; ClientServer and
Fanout do not.

### 2.4 When `next` Is Not Called

| Dispatch | What the caller sees |
| --- | --- |
| `send` | Only that dispatch ends. The sender already received its submission result, so nothing changes for it |
| `request` | Receives a `Rejected` error reply. A missing value does not travel as a `null` success response |
| Fanout subscription | Only that one handler ends; other subscription handlers for the same event still run. Nothing reaches the publisher |

There is no way for a filter to produce a response value of its own. To block a request, do not
call `next`; to change the content of a response, handle it in the handler.

### 2.5 Instances and Dependencies

A new scope opens for each dispatch that runs one handler. The filter and the handler are each
created once within that scope and **share the same `Scoped` service instances.** A value the
filter put in place is the one the handler sees, so per-request state can be carried through a
scoped service. The rule does not change with the DI lifetime the filter type is registered
under.

Fanout creates a dispatch **per matched subscription handler**, not one per event. The filter
therefore runs that many times, and an expensive filter costs proportionally more as subscribers
grow.

## 3. Codecs — Turning a Payload into Bytes

### 3.1 Registration

A codec is enabled in the framework registration. Register none and the default codec is used.

```csharp
--8<-- "framework/languages/dotnet/samples/Bingo/Server/Api/ApiServerHostFactory.cs:doc-codec-register"
```

A payload has to be a DTO the codec can serialize. When the root or an element type is abstract
or an interface, it is a configuration error unless a codec is named explicitly.

### 3.2 When You Need Another Format

For a format other than the default codec — Avro, Thrift, and the like — implement a message
serializer and register it under a content type. A serializer is responsible only for converting
between business objects and bytes; deciding the packet name and selecting the codec stay with
the Framework.

**More than one match for a single payload type is a configuration error.** Keep exactly one
fallback serializer that accepts every type with no type condition, and add as many
type-conditioned serializers as you need as long as they do not overlap.

### 3.3 When to Name One Explicitly

Registering a codec explicitly is a choice you make only when you need it. Register the Protobuf
codec and define DTOs in `.proto` when packet size and encoding cost have to come down, as in a
real-time game. Everything else uses the default.

## 4. The Handler Kinds of Spots and Actors

Which interface to implement depends on what it receives. Whichever it is, it has to match
what was registered in `Configure()`.

Each thing received has one matching interface and one registration call.

| What it receives | Matching registration |
| --- | --- |
| A one-way packet addressed to the Spot | Packet registration |
| A request addressed to the Spot | Packet registration |
| A Logical Multicast subscription event | Subscription registration (specifies channel and topic) |
| A timer tick | Timer registration (specifies name and period, [Timers and workers](36-timer-worker.en.md)) |
| A one-way packet addressed to a member Actor | Actor packet registration |
| A request addressed to a member Actor | Actor packet registration |

The interface names and registration methods per language are as follows.

| What it receives | Interface to implement | Registration |
| --- | --- | --- |
| A one-way packet addressed to the Spot | `IZLinkSpotPacketHandler<TSpot, TMessage>` | `AddPacket<THandler>()` |
| A request addressed to the Spot | `IZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `AddPacket<THandler>()` |
| A Logical Multicast subscription event | `IZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `AddSubscribe<THandler>(channelName, topic)` |
| A timer tick | `IZLinkSpotTimerHandler<TSpot>` | `AddTimer<THandler>(name, period, …)` ([Timers and workers](36-timer-worker.en.md)) |
| A one-way packet addressed to a member Actor | `IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `AddActorPacket<THandler, TActor>()` |
| A request addressed to a member Actor | `IZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `AddActorPacket<THandler, TActor>()` |

A handler takes the target Spot instance as its first argument. It runs inside the Spot, so
it touches state directly, with no lock.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** The
> handler is where the player in the room makes a move. It handles a request addressed to a
> member Actor, receiving the Spot and the Actor together. This is actual code from the repository.

```csharp
--8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorPlaceMarkHandler.cs:doc-actor-packet-handler"
```

The four branches in their minimal form look like this.

```csharp
// A packet addressed to the Spot -- the first argument is the target Spot instance.
public sealed class ChatHandler : IZLinkSpotPacketHandler<GameRoom, Chat>
{
    public ValueTask HandleAsync(
        GameRoom spot,
        Chat message,
        CancellationToken cancellationToken)
    {
        // Touches Spot state directly. No lock needed.
        spot.AppendChat(message.Text);
        return ValueTask.CompletedTask;
    }
}

// A request addressed to the Spot -- the return value is the reply.
public sealed class GetRoomStateHandler
    : IZLinkSpotRequestHandler<GameRoom, GetRoomState, RoomState>
{
    public ValueTask<RoomState> HandleAsync(
        GameRoom spot,
        GetRoomState request,
        CancellationToken cancellationToken)
        => ValueTask.FromResult(spot.Snapshot());
}

// A subscription event -- arrives on the channel/topic registered with AddSubscribe.
public sealed class ScoreHandler : IZLinkSpotSubscriptionHandler<GameRoom, ScoreChanged>
{
    public ValueTask HandleAsync(
        GameRoom spot,
        ScoreChanged @event,
        CancellationToken cancellationToken)
    {
        spot.ApplyScore(@event);
        return ValueTask.CompletedTask;
    }
}

// A packet addressed to a member Actor -- receives the Spot and the Actor together.
public sealed class PlaceMarkHandler
    : IZLinkSpotActorSendHandler<GameRoom, PlayerActor, PlaceMark>
{
    public ValueTask HandleAsync(
        GameRoom spot,
        // The Actor that received this message.
        PlayerActor actor,
        IZLinkMessageContext messageContext,
        PlaceMark message,
        CancellationToken cancellationToken)
    {
        spot.Place(actor.ActorId, message.Cell);
        return ValueTask.CompletedTask;
    }
}
```

An Actor request handler takes the same arguments; the only difference is that its return
value is the reply.

Register handlers in `Configure()` and perform initialization and cleanup in lifecycle
callbacks.

```csharp
public sealed class GameRoom(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        // Registers the Spot send handler.
        Context.Handlers.AddPacket<ChatHandler>();
        Context.Handlers.AddSubscribe<ScoreHandler>(
            "game-events",
            // Registers a Logical Multicast subscription.
            "score.changed");
    }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var create = request.Decode<CreateGame>();
        return ValueTask.FromResult(
            create.Mode is "ranked" or "casual"
                ? ZLinkSpotCreateResponse.Accept(new GameCreated(create.Mode))
                : ZLinkSpotCreateResponse.Reject(new InvalidMode(create.Mode)));
    }

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        // Finishes whatever's needed after creation is approved, before receiving messages.
        return ValueTask.CompletedTask;
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext closing,
        CancellationToken cleanupCancellationToken)
    {
        // Cleans up application resources by the deadline.
        return ValueTask.CompletedTask;
    }
}
```

`OnClosingAsync`'s reason distinguishes explicit close, host shutdown, and relocation out. The
Framework cancels the cleanup token when the `Deadline` runs out.

**Not all three reasons come for every Spot kind.**

| Close reason | Entry | User | Instance | When |
| --- | :---: | :---: | :---: | --- |
| Explicit close | X | O | O | When the application starts a close and the local instance is cleaned up normally |
| Host shutdown | O | O | O | When the host cleans up a local Spot with no relocation |
| Relocation out | X | O | O | After committing the owner to the target, when the source instance is cleaned up |

**Remember the two cases where it's not called.**

- **Not called if close fails.** If Actor membership is still left on a User Spot and the
  explicit close ends in failure, `OnClosingAsync` doesn't run. This is why you shouldn't assume
  "it must have been cleaned up" without checking the close result.
- **The Entry Spot doesn't close when an Actor leaves.** One Actor moving to a different
  Entry Spot isn't the Spot instance being terminated, so it doesn't call the Entry Spot's
  `OnClosingAsync`.

**The Entry Spot itself never relocates.** That's why relocation out never happens to an
Entry Spot. When a host relocates, what the Framework moves is **the Actors belonging to the
Entry Spot** -- the destination's Entry Spot is already created with a new ID and lifetime
when that host starts. So state kept in the Entry Spot doesn't follow the host when it moves
-- **state that needs to move belongs on the Actor or User Spot.**

On host shutdown, the callback runs **while Actor membership and the local instance are
still alive.** Cleanup happens after the callback finishes, so code inside it that reads
member Actors is valid.

## 5. Related Documents

- From writing one handler to calling it — [Channel Messaging](20-channel-messaging.en.md)
- Wiring and target selection per pattern — [How Channels Work](30-channel-patterns.en.md)
- Observing dispatch failures — [Monitoring](26-monitoring.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
