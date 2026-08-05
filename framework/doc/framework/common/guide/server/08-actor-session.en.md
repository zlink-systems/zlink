# 8. Session And Actor Binding

> **The documents that own this chapter's contract** —
> [Session Actor dispatch](../../../common/spec/20-session-actor-dispatch.ko.md) owns the
> behavior, and the
> [per-language STREAM session / bound session public contract](../../../common/spec/server/languages/README.ko.md)
> owns the exact signatures.

Session binding connects a client STREAM session to an exact Actor incarnation. After
binding, the session can relay a client packet to the Actor, and the Actor can push through
the same session.

Binding is independent of the Actor's Spot membership. Even when an Actor relocates to
another Spot or node, `ActorId` and `ObjectGeneration` are preserved and the Framework
updates the binding route.

**The cardinality is open on only one side.** One Session can bind several Actors at
once — one connection can use both a player Actor and a party Actor together. Conversely,
**one Actor is bound to only one session at a time.** Once a new binding is confirmed, the
previous binding becomes invalid, and a late message that arrives for it is rejected.

**Relay doesn't re-query the Location Store.** The session keeps, per Actor, the route it
confirmed at bind time, and sends using that. When an Actor moves, the Framework updates
that stored route after the relocation commits — the application doesn't rebind.

## 1. Binding An Actor After Authentication

Create or find the Actor in the Session handler, then bind the `ActorRef`. Don't pass a
local Actor instance or a target `NodeRid` directly.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the spot that receives an authentication request, creates the player Actor, binds it to
> the session, and sends the reply. Actual code from the repository.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/Handlers/AuthenticatePlaySessionHandler.cs:doc-session-auth"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate_play_session_handler.hpp:doc-session-auth"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/handlers/AuthenticatePlaySessionHandler.java:doc-session-auth"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/handlers/AuthenticatePlaySessionHandler.kt:doc-session-auth"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler.ts:doc-session-auth"
    ```

In its minimal shape, it looks like this.

=== "C#/.NET"

    ```csharp
    public sealed class AuthenticateHandler(IZLinkActorManager actors)
        : IZLinkSessionPacketHandler<IZLinkSessionContext, Authenticate>
    {
        public async ValueTask HandleAsync(
            IZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Authenticate request,
            CancellationToken cancellationToken)
        {
            ActorRef actor = (await actors
                .GetOrCreate(request.PlayerId, "player")
                .Request(new CreatePlayer(request.DisplayName))
                .Async(cancellationToken)) switch
            {
                ZLinkActorCreateResult.Existing value => value.Actor,
                ZLinkActorCreateResult.Created value => value.Actor,
                _ => throw new InvalidOperationException("Player creation was rejected.")
            };

            await context.Actors.BindOrGetAsync(
                actor,
                cancellationToken); // Returns the existing route if the same exact incarnation is already bound.

            await context.Client
                .Reply(new Authenticated(actor.ActorId))
                .Async(cancellationToken); // Submits the current request's one-shot reply.
        }
    }
    ```

=== "C++"

    ```cpp
    // A C++ session branches directly in on_packet instead of registering handlers.
    auto located = actors.get_or_create (sample_names_t::actor_type, request.player_id,
                                         create_player_t{request.display_name});
    if (!located)
        co_return result_t<session_actor_t>::failure (framework_error_kind_t::request_failed,
                                                     "Player actor could not be located.");

    // Returns the existing route if the same exact incarnation is already bound.
    auto actor = co_await actors.bind_or_get (located.value ().ref ()).submit ();

    // Submits the current request's one-shot reply.
    stream.reply_packet (zlink::message_t::from_json (authenticated_t{actor.actor_id ()}))
      .submit ();
    ```

=== "Java"

    ```java
    public CompletionStage<Void> handle(
        ZLinkSessionContext context, ZLinkSessionDispatchContext dispatch, Authenticate request) {
        return actors
            .getOrCreate(request.playerId(), "player")
            .request(new CreatePlayer(request.displayName()))
            .submit()
            // Returns the existing route if the same exact incarnation is already bound.
            .thenCompose(result -> context.actors().bind(requireActor(result)))
            // Submits the current request's one-shot reply.
            .thenRun(() -> context.client()
                .reply(new Authenticated(request.playerId()))
                .submit());
    }

    private static ActorRef requireActor(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Existing existing) return existing.actor();
        if (result instanceof ZLinkActorCreateResult.Created created) return created.actor();
        throw new IllegalStateException("Player creation was rejected.");
    }
    ```

=== "Kotlin"

    ```kotlin
    suspend fun handle(
        context: ZLinkSessionContext, dispatch: ZLinkSessionDispatchContext, request: Authenticate) {
        val result = actors
            .getOrCreate(request.playerId, "player")
            .request(CreatePlayer(request.displayName))
            .submit()
            .await()
        val actor = when (result) {
            is ZLinkActorCreateResult.Existing -> result.actor()
            is ZLinkActorCreateResult.Created -> result.actor()
            else -> error("Player creation was rejected.")
        }

        // Returns the existing route if the same exact incarnation is already bound.
        context.actors().bind(actor).await()

        // Submits the current request's one-shot reply.
        context.client().reply(Authenticated(actor.actorId())).submit().await()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    async handle(
      context: ZLinkSessionContext, dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
      const request = payload.decode<Authenticate>(Object as never);
      const created = await this.actors
        .getOrCreate(request.playerId, 'player')
        .request(createPlayer(request.displayName))
        .submit();

      // Returns the existing route if the same exact incarnation is already bound.
      await context.actors.bindOrGet(requireActor(created));

      // Submits the current request's one-shot reply.
      await context.client.reply(authenticated(request.playerId)).submit();
    }
    ```


`Bind` treats a duplicate bind as an error. For a flow that might already be bound, like a
retried authentication, use `BindOrGet`.

## 2. Relaying A Session Packet To An Actor

Register session-only handlers, such as authentication, in the Session's `Configure()`. An
unhandled packet is handed to the bound Actor.

=== "C#/.NET"

    ```csharp
    public sealed class PlaySession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers
                .AddHandler<AuthenticateHandler>(); // Registers the packet to handle before Actor binding.
        }

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            if (await Context.Handlers.TryHandleAsync(
                    dispatch,
                    payload,
                    cancellationToken))
            {
                return;
            }

            IZLinkSessionActor actor = Context.Actors.Bound.Count == 1
                ? Context.Actors.Bound.Single()
                : throw new InvalidOperationException(
                    "Exactly one actor must be bound before relay.");

            await actor.RelayAsync(
                payload,
                cancellationToken); // Hands the Framework-owned payload to the Actor handler without decoding it.
        }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
            => ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken)
            => ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
            => ValueTask.CompletedTask;
    }
    ```

=== "C++"

    ```cpp
    // A C++ session inherits the interface and branches inside one on_packet.
    // Instead of a handler registry, it directly checks "is this the authenticate packet."
    class play_session_t : public packet_stream_session_t
    {
      public:
        task_t<void> on_packet (stream_t &stream,
                                const stream_dispatch_context_t &dispatch,
                                const zlink::message_t &payload) override
        {
            // Filter out the packet to handle before Actor binding first.
            if (_authenticate.can_handle (dispatch)) {
                auto authenticated = co_await _authenticate.handle (_actors, stream, payload);
                _bound_actor_id = std::string (authenticated.actor_id ());
                co_return;
            }

            auto actor = require_bound_actor ();
            if (!actor)
                co_return;

            // Hands the Framework-owned payload to the Actor handler without decoding it.
            co_await actor.value ().relay (payload);
        }

        task_t<void> on_connected (stream_t &) override { co_return; }
        task_t<void> on_disconnected (stream_t &) override { co_return; }
        task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }
    };
    ```

=== "Java"

    ```java
    public final class PlaySession implements ZLinkSession {
        private final ZLinkSessionContext context;

        @Override
        public void configure() {
            // Registers the packet to handle before Actor binding.
            context.handlers().addHandler(AuthenticateHandler.class);
        }

        @Override
        public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            return context.handlers().tryHandle(context, dispatch, payload).thenCompose(handled -> {
                if (handled) return CompletableFuture.completedFuture(null);
                // Hands the Framework-owned payload to the Actor handler without decoding it.
                return requireSingleBoundActor().relay(payload).thenApply(ignored -> null);
            });
        }

        @Override
        public CompletionStage<Void> onConnected() { return CompletableFuture.completedFuture(null); }

        @Override
        public CompletionStage<Void> onDisconnected() { return CompletableFuture.completedFuture(null); }
    }
    ```

=== "Kotlin"

    ```kotlin
    class PlaySession(private val context: ZLinkSessionContext) : ZLinkSession {

        override fun configure() {
            // Registers the packet to handle before Actor binding.
            context.handlers().addHandler(AuthenticateHandler::class.java)
        }

        override suspend fun onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
            if (context.handlers().tryHandle(context, dispatch, payload).await()) return

            // Hands the Framework-owned payload to the Actor handler without decoding it.
            requireSingleBoundActor().relay(payload).await()
        }

        override suspend fun onConnected() {}
        override suspend fun onDisconnected() {}
    }
    ```

=== "Node/TypeScript"

    ```typescript
    export class PlaySession implements ZLinkSession {
      constructor(private readonly context: ZLinkSessionContext) {}

      configure(): void {
        // Registers the packet to handle before Actor binding.
        this.context.handlers.addHandler(AuthenticateHandler);
      }

      async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
        if (await this.context.handlers.tryHandle(dispatch, payload)) return;

        // Hands the Framework-owned payload to the Actor handler without decoding it.
        await this.requireSingleBoundActor().relay(payload);
      }

      async onConnected(): Promise<void> {}
      async onDisconnected(): Promise<void> {}
    }
    ```


One session can bind several Actors. In that case, the application protocol passes the
ActorId it chose to `Context.Actors.Find(actorId)`. The Framework never picks an arbitrary
Actor on its own.

## 3. Disconnect Notification

The Framework automatically notifies every current binding on a physical STREAM disconnect.
Call it explicitly only to signal a logical disconnect while the connection stays up.

=== "C#/.NET"

    ```csharp
    IZLinkSessionActor? actor = Context.Actors.Find(playerId);
    if (actor is not null)
    {
        await actor.NotifyDisconnectedAsync(cancellationToken);
        // Waits for the OnDisconnectActorAsync callback on the Actor's Spot to complete.
    }
    ```

=== "C++"

    ```cpp
    if (auto actor = _actors.find (player_id)) {
        // Waits for the on_disconnect_actor callback on the Actor's Spot to complete.
        co_await actor->notify_disconnected ();
    }
    ```

=== "Java"

    ```java
    ZLinkSessionActor actor = context.actors().find(playerId);
    if (actor != null) {
        // Waits for the onDisconnectActor callback on the Actor's Spot to complete.
        actor.notifyDisconnected().toCompletableFuture().join();
    }
    ```

=== "Kotlin"

    ```kotlin
    context.actors().find(playerId)?.let { actor ->
        // Waits for the onDisconnectActor callback on the Actor's Spot to complete.
        actor.notifyDisconnected().await()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    const actor = context.actors.find(playerId);
    if (actor !== undefined) {
      // Waits for the onDisconnectActor callback on the Actor's Spot to complete.
      await actor.notifyDisconnected();
    }
    ```


A disconnect doesn't delete the Actor or move it to the Entry Spot. A reconnecting session
can look up the same `ActorRef` again and bind it.

**If notifying one Actor fails, the rest continue.** The Framework fixes a snapshot of the
bindings at the moment the connection drops and notifies each Actor; if one of them fails, or
a callback exceeds its deadline, it doesn't stop notifying the remaining Actors or stop
session cleanup.

**Even if the automatic notification and an explicit call overlap, the callback runs only
once.** The Framework merges two notifications for the same binding, so if the connection
drops right after an explicit call, the Spot's disconnect callback doesn't run twice.

## 4. Pushing From An Actor To The Client

An Actor handler sends a message to the currently bound client through
`Context.BoundSession`.

=== "C#/.NET"

    ```csharp
    public sealed class StateChangedHandler
        : IZLinkSpotActorSendHandler<GameRoom, PlayerActor, StateChanged>
    {
        public async ValueTask HandleAsync(
            GameRoom spot,
            PlayerActor actor,
            IZLinkMessageContext messageContext,
            StateChanged message,
            CancellationToken cancellationToken)
        {
            await actor.Context.BoundSession
                .Send(new GameStateNotify(message.State))
                .Metadata("revision", message.Revision.ToString())
                .Async(cancellationToken); // Waits for local admission on the current bound session.
        }
    }
    ```

=== "C++"

    ```cpp
    // A C++ actor handler is a Spot member function. The Spot arrives as `this`, so there are 3 arguments.
    task_t<void> game_room_t::state_changed (player_actor_t &actor,
                                             const message_context_t &,
                                             const state_changed_t &message)
    {
        // Waits for local admission on the current bound session.
        co_await actor.context ().bound_session ()
          .send (game_state_notify_t{message.state})
          .metadata ("revision", std::to_string (message.revision))
          .submit ();
    }
    ```

=== "Java"

    ```java
    public CompletionStage<Void> handle(
        GameRoom spot, PlayerActor actor, ZLinkMessageContext messageContext, StateChanged message) {
        // Waits for local admission on the current bound session.
        return actor.context().boundSession()
            .send(new GameStateNotify(message.state()))
            .metadata("revision", String.valueOf(message.revision()))
            .submit();
    }
    ```

=== "Kotlin"

    ```kotlin
    suspend fun handle(
        spot: GameRoom, actor: PlayerActor, messageContext: ZLinkMessageContext, message: StateChanged) {
        // Waits for local admission on the current bound session.
        actor.context().boundSession()
            .send(GameStateNotify(message.state))
            .metadata("revision", message.revision.toString())
            .submit()
            .await()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    async handle(
      spot: GameRoom, actor: PlayerActor, messageContext: ZLinkMessageContext, message: StateChanged) {
      // Waits for local admission on the current bound session.
      await actor.context.boundSession
        .send(gameStateNotify(message.state))
        .metadata('revision', String(message.revision))
        .submit();
    }
    ```


A bound session only provides push and disconnect. An Actor's reply to a client request is
handled through the request handler's return value.

## 5. Error-Handling Standard

| Situation | Result |
|---|---|
| The Actor doesn't exist or isn't Ready | The bind ends with a typed framework error. |
| `ObjectGeneration` differs | A stale ActorRef is never bound to a different incarnation. |
| An Actor relocation seal is in progress | Ends with `ActorMoving`, with no hidden retry. |
| An Actor relocates after binding | The Framework updates the route without rebinding the session. |
| Session disconnect | The Actor and its Spot membership are preserved. |
| A reply arrives after the session has closed | Discarded. Never used as the reply for a new session or a new binding. |
| A timeout/route failure after a relay | Never auto-resent to a different Actor, new owner, or different node. |

`ActorRef.MeshName` and `NodeRid` are a snapshot of the initial control route. The
application doesn't assemble a stale route on its own — it re-obtains the current ref through
the actor manager's lookup call.

## 6. Related Documents

- Runnable verification examples for this chapter's contract: `13. Interface Catalog`
  chapter §5 — the verification class `StreamContracts`
- The STREAM node and session lifecycle: [STREAM](09-stream.ko.md)
- Actor creation and Spot join: [Actor And Spot](07-actor-spot.ko.md)
