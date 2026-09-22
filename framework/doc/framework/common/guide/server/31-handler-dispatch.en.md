# Handlers and Message Processing

The code in this chapter comes from the [tutorial README](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/tutorial/README.md) and the [`Bingo`](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/samples/Bingo/README.md), [`DeliveryDispatch`](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/samples/DeliveryDispatch/README.md), [`TicTacToe`](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/samples/TicTacToe/README.md), and [`ZoneWorld`](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/samples/ZoneWorld/README.md) sample READMEs. Download the [examples repository](https://github.com/zlink-systems/zlink-<language>-examples/blob/main/tutorial/README.md) and follow each README's Download, Build and Run sections to reproduce the results below.

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

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Program.cs:doc-multi-channel-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/ZoneWorld/Server/ZoneNode/main.cpp:doc-multi-channel-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java:doc-multi-channel-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-multi-channel-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/zone-node-module.ts:doc-multi-channel-register"
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

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs:doc-explicit-packet-name"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/DeliveryDispatch/Server/CourierActorNode/main.cpp:doc-explicit-packet-name"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/DeliveryDispatch/Shared/src/main/java/systems/zlink/samples/deliverydispatch/shared/contracts/Messages.java:doc-explicit-packet-name"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Shared/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/shared/contracts/Messages.kt:doc-explicit-packet-name"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-explicit-packet-name"
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

<iframe class="zlink-diagram" src="/common/diagrams/31-filter-scope-en.html" title="A filter wraps the messages a node receives" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/31-filter-scope-en.html" target="_blank">↗ View larger</a></p>

What it wraps is the heart of this feature. A filter wraps **only the messages a node receives**,
so applying the same code to a Spot or Actor handler means placing it there separately. The
sections below take writing, registration order, and scope in turn.

### 2.1 Writing a Filter

Implement the filter interface and call `next`. The handler does not run unless `next` is
called. For the exact names, read your language's tab below.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Dispatch/CallLogFilter.cs:filter-implementation"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/dispatch/call_log_filter.hpp:filter-implementation"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/dispatch/CallLogFilter.java:filter-implementation"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/dispatch/CallLogFilter.kt:filter-implementation"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/Dispatch/call-log-filter.ts:filter-implementation"
    ```

### 2.2 Registration Order Is Execution Order

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:filter-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:filter-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:filter-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:filter-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:filter-register"
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

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/Bingo/Server/Api/ApiServerHostFactory.cs:doc-codec-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/Bingo/Server/Api/api_server_framework.hpp:doc-codec-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/Bingo/Server/Api/src/main/java/systems/zlink/samples/bingo/server/api/ApiServerApplication.java:doc-codec-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/Bingo/Server/Api/src/main/kotlin/systems/zlink/samples/kotlin/bingo/server/api/ApiServerApplication.kt:doc-codec-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/Bingo.Ts/Server/Api/bingo-api-module.ts:doc-codec-register"
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

=== "C#/.NET"

    | What it receives | Interface to implement | Registration |
    | --- | --- | --- |
    | A one-way packet addressed to the Spot | `IZLinkSpotPacketHandler<TSpot, TMessage>` | `AddPacket<THandler>()` |
    | A request addressed to the Spot | `IZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `AddPacket<THandler>()` |
    | A Logical Multicast subscription event | `IZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `AddSubscribe<THandler>(channelName, topic)` |
    | A timer tick | `IZLinkSpotTimerHandler<TSpot>` | `AddTimer<THandler>(name, period, …)` ([Timers and workers](36-timer-worker.en.md)) |
    | A one-way packet addressed to a member Actor | `IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `AddActorPacket<THandler, TActor>()` |
    | A request addressed to a member Actor | `IZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `AddActorPacket<THandler, TActor>()` |

=== "C++"

    C++ registers a Spot member function instead of a handler class. The registration call
    itself is the kind.

    | What it receives | Registration |
    | --- | --- |
    | A one-way packet addressed to the Spot | `add_handler<&TSpot::method> ()` |
    | A request addressed to the Spot | `add_handler<&TSpot::method> ()` (the return value is the reply) |
    | A Logical Multicast subscription event | `add_subscribe<&TSpot::method> (channel_name, topic)` |
    | A timer tick | `add_timer<THandler> (name, period, options)` ([Timers and workers](36-timer-worker.en.md)) |
    | A one-way packet addressed to a member Actor | `add_actor_send<&TSpot::method> ()` |
    | A request addressed to a member Actor | `add_actor_request<&TSpot::method> ()` |

=== "Java"

    | What it receives | Interface to implement | Registration |
    | --- | --- | --- |
    | A one-way packet addressed to the Spot | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `addHandler(THandler.class)` |
    | A request addressed to the Spot | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `addHandler(THandler.class)` |
    | A Logical Multicast subscription event | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `@ZLinkSpotSubscription(topic)` on the handler + `addHandler(THandler.class)` |
    | A timer tick | `ZLinkSpotTimerHandler<TSpot>` | `context.addTimer(name, period, THandler.class, options)` ([Timers and workers](36-timer-worker.en.md)) |
    | A one-way packet addressed to a member Actor | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `@ZLinkSpotActorSend` on the handler + `addHandler(THandler.class)` |
    | A request addressed to a member Actor | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `@ZLinkSpotActorRequest` on the handler + `addHandler(THandler.class)` |

    **There's a single registration method, `addHandler`.** What kind of handler it is comes
    from the interface it implements and its annotation. The packet name comes from the
    message type's `@ZLinkPacket`.

=== "Kotlin"

    | What it receives | Interface to implement | Registration |
    | --- | --- | --- |
    | A one-way packet addressed to the Spot | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `addHandler(THandler::class.java)` |
    | A request addressed to the Spot | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `addHandler(THandler::class.java)` |
    | A Logical Multicast subscription event | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `@ZLinkSpotSubscription(topic)` on the handler + `addHandler(THandler::class.java)` |
    | A timer tick | `ZLinkSpotTimerHandler<TSpot>` | `context.addTimer(name, period, THandler::class.java, options)` ([Timers and workers](36-timer-worker.en.md)) |
    | A one-way packet addressed to a member Actor | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `@ZLinkSpotActorSend` on the handler + `addHandler(THandler::class.java)` |
    | A request addressed to a member Actor | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `@ZLinkSpotActorRequest` on the handler + `addHandler(THandler::class.java)` |

    **Uses the Java surface as-is.** There's a single registration method, `addHandler`, and
    what kind of handler it is comes from the interface it implements and its annotation.

=== "Node/TypeScript"

    | What it receives | Interface to implement | Registration |
    | --- | --- | --- |
    | A one-way packet addressed to the Spot | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `addPacket(THandler)` |
    | A request addressed to the Spot | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `addPacket(THandler)` |
    | A Logical Multicast subscription event | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `addSubscribe(THandler, channelName, topic)` |
    | A timer tick | `ZLinkSpotTimerHandler<TSpot>` | `addTimer(name, periodMs, THandler, options)` ([Timers and workers](36-timer-worker.en.md)) |
    | A one-way packet addressed to a member Actor | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `addActorPacket(THandler)` |
    | A request addressed to a member Actor | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `addActorPacket(THandler)` |

A handler takes the target Spot instance as its first argument. It runs inside the Spot, so
it touches state directly, with no lock.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** The
> handler is where the player in the room makes a move. It handles a request addressed to a
> member Actor, receiving the Spot and the Actor together. This is actual code from the repository.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorPlaceMarkHandler.cs:doc-actor-packet-handler"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_place_mark_handler.hpp:doc-actor-packet-handler"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/handlers/PlayActorPlaceMarkHandler.java:doc-actor-packet-handler"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/handlers/PlayActorPlaceMarkHandler.kt:doc-actor-packet-handler"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler.ts:doc-actor-packet-handler"
    ```

The four branches in their minimal form look like this.

=== "C#/.NET"

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

=== "C++"

    ```cpp
    // C++ registers a Spot member function instead of a handler class. The first argument is the target Spot.
    class game_room_t : public spot_t<player_actor_t>
    {
      public:
        // A packet addressed to the Spot.
        task_t<void> chat (const chat_t &message)
        {
            // Touches Spot state directly. No lock needed.
            append_chat (message.text);
            co_return;
        }

        // A request addressed to the Spot -- the return value is the reply.
        room_state_t get_room_state (const get_room_state_t &) { return snapshot (); }

        // A subscription event -- arrives on the channel/topic registered with add_subscribe.
        task_t<void> score (const score_changed_t &event)
        {
            apply_score (event);
            co_return;
        }

        // A packet addressed to a member Actor -- receives the Spot and the Actor together.
        // The Actor that received this message.
        task_t<void> place_mark (player_actor_t &actor,
                                 message_context_t &,
                                 const place_mark_t &message)
        {
            place (actor.actor_id, message.cell);
            co_return;
        }
    };
    ```

=== "Java"

    ```java
    // A packet addressed to the Spot -- the first argument is the target Spot instance.
    public final class ChatHandler implements ZLinkSpotPacketHandler<GameRoom, Chat> {
        @Override
        public CompletionStage<Void> handle(GameRoom spot, Chat message) {
            // Touches Spot state directly. No lock needed.
            spot.appendChat(message.text());
            return CompletableFuture.completedFuture(null);
        }
    }

    // A request addressed to the Spot -- the return value is the reply.
    public final class GetRoomStateHandler
        implements ZLinkSpotRequestHandler<GameRoom, GetRoomState, RoomState> {
        @Override
        public CompletionStage<RoomState> handle(GameRoom spot, GetRoomState request) {
            return CompletableFuture.completedFuture(spot.snapshot());
        }
    }

    // A subscription event -- arrives on the topic in @ZLinkSpotSubscription.
    public final class ScoreHandler
        implements ZLinkSpotSubscriptionHandler<GameRoom, ScoreChanged> {
        @Override
        public CompletionStage<Void> handle(GameRoom spot, ScoreChanged event) {
            spot.applyScore(event);
            return CompletableFuture.completedFuture(null);
        }
    }

    // A packet addressed to a member Actor -- receives the Spot and the Actor together.
    public final class PlaceMarkHandler
        implements ZLinkSpotActorSendHandler<GameRoom, PlayerActor, PlaceMark> {
        @Override
        public CompletionStage<Void> handle(
            GameRoom spot,
            // The Actor that received this message.
            PlayerActor actor,
            ZLinkMessageContext messageContext,
            PlaceMark message) {
            spot.place(actor.actorId(), message.cell());
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    // A packet addressed to the Spot -- the first argument is the target Spot instance.
    class ChatHandler : ZLinkSpotPacketHandler<GameRoom, Chat> {
        override suspend fun handle(spot: GameRoom, message: Chat) {
            // Touches Spot state directly. No lock needed.
            spot.appendChat(message.text)
        }
    }

    // A request addressed to the Spot -- the return value is the reply.
    class GetRoomStateHandler : ZLinkSpotRequestHandler<GameRoom, GetRoomState, RoomState> {
        override suspend fun handle(spot: GameRoom, request: GetRoomState): RoomState = spot.snapshot()
    }

    // A subscription event -- arrives on the topic in @ZLinkSpotSubscription.
    class ScoreHandler : ZLinkSpotSubscriptionHandler<GameRoom, ScoreChanged> {
        override suspend fun handle(spot: GameRoom, event: ScoreChanged) {
            spot.applyScore(event)
        }
    }

    // A packet addressed to a member Actor -- receives the Spot and the Actor together.
    class PlaceMarkHandler : ZLinkSpotActorSendHandler<GameRoom, PlayerActor, PlaceMark> {
        override suspend fun handle(
            spot: GameRoom,
            // The Actor that received this message.
            actor: PlayerActor,
            messageContext: ZLinkMessageContext,
            message: PlaceMark,
        ) {
            spot.place(actor.actorId(), message.cell)
        }
    }
    ```

=== "Node/TypeScript"

    ```typescript
    // A packet addressed to the Spot -- the first argument is the target Spot instance.
    export class ChatHandler implements ZLinkSpotPacketHandler<GameRoom, Chat> {
      async handle(spot: GameRoom, message: Chat): Promise<void> {
        // Touches Spot state directly. No lock needed.
        spot.appendChat(message.text);
      }
    }

    // A request addressed to the Spot -- the return value is the reply.
    export class GetRoomStateHandler
      implements ZLinkSpotRequestHandler<GameRoom, GetRoomState, RoomState> {
      async handle(spot: GameRoom, request: GetRoomState): Promise<RoomState> {
        return spot.snapshot();
      }
    }

    // A subscription event -- arrives on the channel/topic registered with addSubscribe.
    export class ScoreHandler implements ZLinkSpotSubscriptionHandler<GameRoom, ScoreChanged> {
      async handle(spot: GameRoom, event: ScoreChanged): Promise<void> {
        spot.applyScore(event);
      }
    }

    // A packet addressed to a member Actor -- receives the Spot and the Actor together.
    export class PlaceMarkHandler
      implements ZLinkSpotActorSendHandler<GameRoom, PlayerActor, PlaceMark> {
      async handle(
        spot: GameRoom,
        // The Actor that received this message.
        actor: PlayerActor,
        messageContext: ZLinkMessageContext,
        message: PlaceMark
      ): Promise<void> {
        spot.place(actor.actorId, message.cell);
      }
    }
    ```


An Actor request handler takes the same arguments; the only difference is that its return
value is the reply.

Register handlers in `Configure()` and perform initialization and cleanup in lifecycle
callbacks.

=== "C#/.NET"

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

=== "C++"

    ```cpp
    class game_room_t : public spot_t<player_actor_t>
    {
      public:
        spot_context_t &context () noexcept override { return _context; }

        void configure () override
        {
            // Registers the Spot send handler.
            _context.handlers ().add_handler<&game_room_t::chat> ();
            _context.handlers ().add_subscribe<&game_room_t::score> (
              // Registers a Logical Multicast subscription.
              "game-events", "score.changed");
        }

        task_t<spot_create_response_t> on_create (const message_t &request) override
        {
            const auto create = request.decode<create_game_t> ();
            co_return (create.mode == "ranked" || create.mode == "casual")
                      ? spot_create_response_t::accept (game_created_t{create.mode})
                      : spot_create_response_t::reject (invalid_mode_t{create.mode});
        }

        task_t<void> on_initialize () override
        {
            // Finishes whatever's needed after creation is approved, before receiving messages.
            co_return;
        }

        task_t<void> on_closing (const spot_closing_context_t &) override
        {
            // Cleans up application resources by the deadline.
            co_return;
        }

      private:
        spot_context_t _context;
    };
    ```

=== "Java"

    ```java
    public final class GameRoom implements ZLinkSpot {
        private final ZLinkSpotContext context;

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            // Registers the Spot send handler.
            context.handlers().addHandler(ChatHandler.class);
            // The subscription topic is set by @ZLinkSpotSubscription on ScoreHandler.
            context.handlers().addHandler(ScoreHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            CreateGame create = request.decode(CreateGame.class);
            boolean known = "ranked".equals(create.mode()) || "casual".equals(create.mode());
            return CompletableFuture.completedFuture(known
                ? ZLinkSpotCreateResponse.accept(new GameCreated(create.mode()))
                : ZLinkSpotCreateResponse.reject(new InvalidMode(create.mode())));
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            // Finishes whatever's needed after creation is approved, before receiving messages.
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            // Cleans up application resources by the deadline.
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    class GameRoom(private val spotContext: ZLinkSpotContext) : ZLinkSpot {

        override fun context(): ZLinkSpotContext = spotContext

        override fun configure() {
            // Registers the Spot send handler.
            spotContext.handlers().addHandler(ChatHandler::class.java)
            // The subscription topic is set by @ZLinkSpotSubscription on ScoreHandler.
            spotContext.handlers().addHandler(ScoreHandler::class.java)
        }

        override suspend fun onCreate(request: ZLinkMessage): ZLinkSpotCreateResponse {
            val create = request.decode(CreateGame::class.java)
            return if (create.mode in setOf("ranked", "casual"))
                ZLinkSpotCreateResponse.accept(GameCreated(create.mode))
            else ZLinkSpotCreateResponse.reject(InvalidMode(create.mode))
        }

        override suspend fun onInitialize() {
            // Finishes whatever's needed after creation is approved, before receiving messages.
        }

        override suspend fun onClosing(closing: ZLinkSpotClosingContext) {
            // Cleans up application resources by the deadline.
        }
    }
    ```

=== "Node/TypeScript"

    ```typescript
    export class GameRoom implements ZLinkSpot {
      readonly context!: ZLinkSpotContext;

      configure(): void {
        // Registers the Spot send handler.
        this.context.handlers.addPacket(ChatHandler);
        this.context.handlers.addSubscribe(
          ScoreHandler,
          'game-events',
          // Registers a Logical Multicast subscription.
          'score.changed');
      }

      async onCreate(request: ZLinkMessage): Promise<ZLinkSpotCreateResponse> {
        const create = request.decode<CreateGame>(Object as never);
        return create.mode === 'ranked' || create.mode === 'casual'
          ? ZLinkSpotCreateResponse.accept(gameCreated(create.mode))
          : ZLinkSpotCreateResponse.reject(invalidMode(create.mode));
      }

      async onInitialize(): Promise<void> {
        // Finishes whatever's needed after creation is approved, before receiving messages.
      }

      async onClosing(closing: ZLinkSpotClosingContext): Promise<void> {
        // Cleans up application resources by the deadline.
      }
    }
    ```


`OnClosing`'s reason distinguishes explicit close, host shutdown, and relocation out. The
Framework cancels the cleanup token when the `Deadline` runs out.

**Not all three reasons come for every Spot kind.**

| Close reason | Entry | User | Instance | When |
| --- | :---: | :---: | :---: | --- |
| Explicit close | X | O | O | When the application starts a close and the local instance is cleaned up normally |
| Host shutdown | O | O | O | When the host cleans up a local Spot with no relocation |
| Relocation out | X | O | O | After committing the owner to the target, when the source instance is cleaned up |

**Remember the two cases where it's not called.**

- **Not called if close fails.** If Actor membership is still left on a User Spot and the
  explicit close ends in failure, `OnClosing` doesn't run. This is why you shouldn't assume
  "it must have been cleaned up" without checking the close result.
- **The Entry Spot doesn't close when an Actor leaves.** One Actor moving to a different
  Entry Spot isn't the Spot instance being terminated, so it doesn't call the Entry Spot's
  `OnClosing`.

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
