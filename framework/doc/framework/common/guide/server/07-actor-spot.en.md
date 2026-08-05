# 7. Actor And Spot

> **The documents that own this chapter's contract** —
> [Actor Model](../../../common/spec/14-actor-model.ko.md) and
> [Spot And Actor Membership](../../../common/spec/15-spot-actor.ko.md) own the behavior,
> and the
> [per-language Actor/Spot public contract](../../../common/spec/server/languages/README.ko.md)
> owns the exact signatures.

An Actor is a stateful object found by a global string `ActorId`. Right after creation, it
exists in the Object Server's Entry Spot. Once an application handler schedules a join, it
moves to a User Spot.

An Actor's location and its client session binding are separate pieces of state. An Actor's
and Spot's membership persists even when no client is connected. Session binding is covered
in [the next document](08-actor-session.ko.md).

## 1. Registration

Register an Entry Spot and an Actor factory together on the Object Server. Any `Serving`
node that registers `actorType` becomes a creation candidate.

Below is the Play server registration from the
[Bingo sample](../../../common/sample/bingo/README.en.md).

=== "C#/.NET"

    ```csharp
    mesh.Objects().Server()
        .AddEntrySpot<BingoEntrySpot>()
        .AddActorFactory<PlayerActor, PlayerActorFactory>(
            SampleNames.PlayerActorType,
            factory => factory
                .PreserveStateWith<PlayerActorRelocationAdapter>());
    ```

=== "C++"

    ```cpp
    mesh.set_object_role (object_role_t::server)
      .add_entry_spot<bingo_entry_spot_t> (
        [] (entry_spot_context_t context) {
            return std::make_shared<bingo_entry_spot_t> (std::move (context));
        })
      .add_actor_factory<player_actor_t, player_actor_factory_t> (
        sample_names_t::player_actor_type,
        std::make_shared<player_actor_factory_t> (),
        [] (auto &factory) {
            factory.template preserve_state_with<player_actor_relocation_adapter_t> ();
        });
    ```

=== "Java"

    ```java
    mesh.objects().server()
        .addEntrySpot(BingoEntrySpot.class)
        .addActorFactory(
            SampleNames.PlayerActorType,
            PlayerActor.class,
            PlayerActorFactory.class,
            factory -> factory.preserveStateWith(PlayerActorRelocationAdapter.class));
    ```

=== "Kotlin"

    ```kotlin
    mesh.objects().server()
        .addEntrySpot(BingoEntrySpot::class.java)
        .addActorFactory(
            SampleNames.PlayerActorType,
            PlayerActor::class.java,
            PlayerActorFactory::class.java,
        ) { factory -> factory.preserveStateWith(PlayerActorRelocationAdapter::class.java) }
    ```

=== "Node/TypeScript"

    ```typescript
    mesh.objects().server()
      .addEntrySpot(BingoEntrySpot)
      .addActorFactory(
        SampleNames.playerActorType,
        PlayerActorFactory,
        (factory) => factory.preserveStateWith(PlayerActorRelocationAdapter));
    ```


The relocation policy is fixed once, at factory registration, and doesn't change while
running. This policy applies both when an Actor joins another node's Spot and when it moves
via host `Relocate`.

| Policy | How it's recreated on another node |
| --- | --- |
| `DisableRelocation()` | Refuses before a cross-node move even starts. If this target remains, host relocation can't complete. |
| `RecreateOnRelocation()` | Creates a new instance with the same logical identity. Pending messages and timers are preserved, but application state isn't restored. |
| `PreserveStateWith<TAdapter>()` | Restores the `byte[]` the adapter saved onto the new instance. The Framework queue and timers are preserved as well. |

## 2. Creating An Actor

`Create` fails if the same ActorId already exists. `GetOrCreate` returns `Existing` if a
Ready Actor of the same type already exists. The caller never specifies the target node.

=== "C#/.NET"

    ```csharp
    ZLinkActorCreateResult result = await actors
        .GetOrCreate(playerId, "player")
        .InMesh("play")
        .Request(new CreatePlayer(displayName))
        .Timeout(TimeSpan.FromSeconds(10))
        .Async(cancellationToken);

    ActorRef actor = result switch
    {
        ZLinkActorCreateResult.Existing value => value.Actor,
        ZLinkActorCreateResult.Created value => value.Actor,
        ZLinkActorCreateResult.Rejected =>
            throw new InvalidOperationException("Player creation was rejected.")
    };
    ```

=== "C++"

    ```cpp
    auto result = co_await actors
                    .get_or_create ("player", player_id, create_player_t{display_name})
                    .in_mesh ("play")
                    .timeout (std::chrono::seconds (10))
                    .submit ();

    if (!result)
        throw std::runtime_error ("Player creation was rejected.");
    auto actor = result.value ().ref ();
    ```

=== "Java"

    ```java
    ZLinkActorCreateResult result = actors
        .getOrCreate(playerId, "player")
        .inMesh("play")
        .request(new CreatePlayer(displayName))
        .timeout(Duration.ofSeconds(10))
        .submit()
        .toCompletableFuture().join();

    ActorRef actor;
    if (result instanceof ZLinkActorCreateResult.Existing existing) actor = existing.actor();
    else if (result instanceof ZLinkActorCreateResult.Created created) actor = created.actor();
    else throw new IllegalStateException("Player creation was rejected.");
    ```

=== "Kotlin"

    ```kotlin
    val result = actors
        .getOrCreate(playerId, "player")
        .inMesh("play")
        .request(CreatePlayer(displayName))
        .timeout(Duration.ofSeconds(10))
        .submit()
        .await()

    val actor = when (result) {
        is ZLinkActorCreateResult.Existing -> result.actor()
        is ZLinkActorCreateResult.Created -> result.actor()
        else -> error("Player creation was rejected.")
    }
    ```

=== "Node/TypeScript"

    ```typescript
    const result = await actors
      .getOrCreate(playerId, 'player')
      .inMesh('play')
      .request(createPlayer(displayName))
      .timeout(10_000)
      .submit();

    if (result.status === 'rejected') throw new Error('Player creation was rejected.');
    const actor = result.actor;
    ```


`ActorRef` carries the exact incarnation and the owner route as of the lookup. It's used for
session binding or exact destroy. Ordinary Actor messaging uses only the ActorId.

=== "C#/.NET"

    ```csharp
    ActorRef? current = await actors.FindAsync(playerId, cancellationToken);
    SpotRef? currentSpot = await actors.FindSpotAsync(playerId, cancellationToken);

    if (current is { } exact)
    {
        await actors.DestroyAsync(exact, cancellationToken); // Doesn't terminate an Actor whose generation differs.
    }
    ```

=== "C++"

    ```cpp
    auto current = co_await actors.find (player_id);
    auto current_spot = co_await actors.find_spot (player_id);

    if (current) {
        // Doesn't terminate an Actor whose generation differs.
        co_await actors.destroy (current.value ());
    }
    ```

=== "Java"

    ```java
    Optional<ActorRef> current = actors.find(playerId).toCompletableFuture().join();
    Optional<SpotRef> currentSpot = actors.findSpot(playerId).toCompletableFuture().join();

    current.ifPresent(actor ->
        // Doesn't terminate an Actor whose generation differs.
        actors.destroy(actor).toCompletableFuture().join());
    ```

=== "Kotlin"

    ```kotlin
    // Since the Java surface returns an Optional, Kotlin receives it with orElse(null).
    val current = actors.find(playerId).await().orElse(null)
    val currentSpot = actors.findSpot(playerId).await().orElse(null)

    if (current != null) {
        // Doesn't terminate an Actor whose generation differs.
        actors.destroy(current).await()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    const current = await actors.find(playerId);
    const currentSpot = await actors.findSpot(playerId);

    if (current !== undefined) {
      // Doesn't terminate an Actor whose generation differs.
      await actors.destroy(current);
    }
    ```


An Actor can only be terminated from the Entry Spot. If it's in a User Spot, finish an Entry
Spot join first.

## 3. Entry Spot

The Entry Spot accepts or rejects an Actor creation request, and handles the lifecycle of an
Actor joining and leaving.

A **membership callback** is a lifecycle callback the Framework calls when an Actor becomes
or stops being a member of this Spot. The Entry Spot has four.

| Callback | When it's called |
| --- | --- |
| `OnCreateActor` | When a new Actor takes this Entry Spot as its first membership. Decides accept/reject |
| `OnJoinedActor` | Once the commit finishes for an Actor that was in another Spot coming into this Entry Spot |
| `OnLeaveActor` | Once the commit finishes for an Actor that was in this Entry Spot leaving to another Spot |
| `OnDisconnectActor` | When the client connection for an Actor belonging to this Entry Spot drops |

These callbacks aren't called when an Actor is restored into another node's Entry Spot via
relocation. Relocation keeps membership exactly as it is and only moves the execution
location, so from the application's point of view it's not an event of "coming in" or
"going out."

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the Entry Spot a player first enters. Actual code from the repository.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs:doc-entry-spot"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp:doc-entry-spot"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.java:doc-entry-spot"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.kt:doc-entry-spot"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts:doc-entry-spot"
    ```

In its minimal shape, it looks like this.

=== "C#/.NET"

    ```csharp
    // <PlayerActor> — the Actor type this Entry Spot manages membership for.
    // Specifying this type is what gives you the membership callbacks below.
    public sealed class PlayEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<PlayerActor>
    {
        // Exposes the context the Framework passed into the constructor, as-is. Use this
        // property to access handler registration, Actor termination, and outbound calls.
        public IZLinkEntrySpotContext Context { get; } = context;

        // Called once when the Spot instance is prepared. A handler registered here
        // handles a packet addressed to an Actor that belongs to this Entry Spot.
        public void Configure()
        {
            // JoinGameHandler receives a JoinGame packet addressed to a PlayerActor.
            Context.Handlers
                .AddActorPacket<JoinGameHandler, PlayerActor>();
        }

        // Called when a new Actor takes this Entry Spot as its first membership.
        // The return value decides whether to create this Actor — this Spot is the admission gate.
        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            PlayerActor actor,          // A new Actor instance, not yet published.
            ZLinkMessage createRequest, // The value sent via GetOrCreate/Create's .Request(...).
            CancellationToken cancellationToken)
        {
            var request = createRequest.Decode<CreatePlayer>();
            actor.SetDisplayName(request.DisplayName); // The Actor owns its own initial state.

            // Accept() makes the Actor Ready; Reject(...) cancels the creation.
            return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
        }

        // Called once the commit finishes for an Actor that was in a User Spot returning to this Entry Spot.
        // Not called on initial creation or relocation restore.
        public ValueTask OnJoinedActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
            => ValueTask.CompletedTask; // This sample only receives the notification and has nothing else to do.

        // Called after the commit for an Actor that was in this Entry Spot leaving to a User Spot.
        // Doesn't mean the Actor disappeared — it means membership moved.
        public ValueTask OnLeaveActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
            => ValueTask.CompletedTask;
    }
    ```

=== "C++"

    ```cpp
    // <player_actor_t> — the Actor type this Entry Spot manages membership for.
    class play_entry_spot_t : public entry_spot_t<player_actor_t>
    {
      public:
        entry_spot_context_t &context () noexcept override { return _context; }

        // Called once when the Spot instance is prepared. A handler registered here
        // handles a packet addressed to an Actor that belongs to this Entry Spot.
        void configure () override
        {
            // C++ registers a member function directly instead of a handler class.
            _context.handlers ().add_actor_send<&play_entry_spot_t::join_game> ();
        }

        // Called when a new Actor takes this Entry Spot as its first membership.
        // The return value decides whether to create this Actor — this Spot is the admission gate.
        task_t<actor_create_response_t>
        on_create_actor (player_actor_t &actor, const message_t &create_request) override
        {
            // The Actor owns its own initial state.
            actor.apply_player (create_request.decode<create_player_t> ());
            // reject(...) cancels the creation.
            co_return actor_create_response_t::accept ();
        }

        // Called once the commit finishes for an Actor that was in a User Spot returning to this Entry Spot.
        // Not called on initial creation or relocation restore.
        task_t<void> on_actor_joined (player_actor_t &) override { co_return; }

        // Called after the commit for an Actor that was in this Entry Spot leaving to a User Spot.
        // Doesn't mean the Actor disappeared — it means membership moved.
        task_t<void> on_leave_actor (player_actor_t &) override { co_return; }

      private:
        entry_spot_context_t _context;
    };
    ```

=== "Java"

    ```java
    // <PlayerActor> — the Actor type this Entry Spot manages membership for.
    public final class PlayEntrySpot implements ZLinkEntrySpot<PlayerActor> {
        private final ZLinkEntrySpotContext context;

        // Exposes the context the Framework passed into the constructor, as-is.
        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        // Called once when the Spot instance is prepared.
        @Override
        public void configure() {
            // JoinGameHandler receives a JoinGame packet addressed to a PlayerActor.
            // The @ZLinkSpotActorSend on the handler decides which kind it is.
            context.handlers().addHandler(JoinGameHandler.class);
        }

        // Called when a new Actor takes this Entry Spot as its first membership.
        // The return value decides whether to create this Actor — this Spot is the admission gate.
        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
            PlayerActor actor, ZLinkMessage createRequest) {
            actor.setDisplayName(createRequest.decode(CreatePlayer.class).displayName());
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
        }

        // Called once the commit finishes for an Actor that was in a User Spot returning.
        @Override
        public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        // Called after the commit for an Actor leaving to a User Spot.
        @Override
        public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    // <PlayerActor> — the Actor type this Entry Spot manages membership for.
    class PlayEntrySpot(private val entryContext: ZLinkEntrySpotContext) : ZLinkEntrySpot<PlayerActor> {

        override fun context(): ZLinkEntrySpotContext = entryContext

        // Called once when the Spot instance is prepared.
        override fun configure() {
            // JoinGameHandler receives a JoinGame packet addressed to a PlayerActor.
            // The @ZLinkSpotActorSend on the handler decides which kind it is.
            entryContext.handlers().addHandler(JoinGameHandler::class.java)
        }

        // Called when a new Actor takes this Entry Spot as its first membership.
        // The return value decides whether to create this Actor — this Spot is the admission gate.
        override suspend fun onCreateActor(
            actor: PlayerActor, createRequest: ZLinkMessage): ZLinkActorCreateResponse {
            actor.setDisplayName(createRequest.decode(CreatePlayer::class.java).displayName)
            return ZLinkActorCreateResponse.accept()
        }

        // Called once the commit finishes for an Actor that was in a User Spot returning.
        override suspend fun onJoinedActor(actor: PlayerActor) {}

        // Called after the commit for an Actor leaving to a User Spot.
        override suspend fun onLeaveActor(actor: PlayerActor) {}
    }
    ```

=== "Node/TypeScript"

    ```typescript
    // <PlayerActor> — the Actor type this Entry Spot manages membership for.
    export class PlayEntrySpot implements ZLinkEntrySpot<PlayerActor> {
      readonly context!: ZLinkEntrySpotContext<PlayerActor>;

      // Called once when the Spot instance is prepared.
      configure(): void {
        // JoinGameHandler receives a JoinGame packet addressed to a PlayerActor.
        this.context.handlers.addActorPacket(JoinGameHandler);
      }

      // Called when a new Actor takes this Entry Spot as its first membership.
      // The return value decides whether to create this Actor — this Spot is the admission gate.
      async onCreateActor(
        actor: PlayerActor, createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
        actor.setDisplayName(createRequest.decode<CreatePlayer>(Object as never).displayName);
        return ZLinkActorCreateResponse.accept();
      }

      // Called once the commit finishes for an Actor that was in a User Spot returning.
      async onJoinedActor(actor: PlayerActor): Promise<void> {}

      // Called after the commit for an Actor leaving to a User Spot.
      async onLeaveActor(actor: PlayerActor): Promise<void> {}
    }
    ```


It's safer for an Entry Spot not to keep per-Actor application state of its own. The Actor
owns its state; the Entry Spot only provides handlers and the membership lifecycle.

To terminate an Actor, first return it to the Entry Spot, then pass the current Actor
instance to the Entry Spot context's actor-destroy call.

=== "C#/.NET"

    ```csharp
    await Context.DestroyActorAsync(
        actor,
        cancellationToken); // The Entry Spot requests termination of the current Actor.
    ```

=== "C++"

    ```cpp
    // The Entry Spot requests termination of the current Actor.
    co_await _context.destroy_actor (actor);
    ```

=== "Java"

    ```java
    // The Entry Spot requests termination of the current Actor.
    context.destroyActor(actor).toCompletableFuture().join();
    ```

=== "Kotlin"

    ```kotlin
    // The Entry Spot requests termination of the current Actor.
    entryContext.destroyActor(actor).await()
    ```

=== "Node/TypeScript"

    ```typescript
    // The Entry Spot requests termination of the current Actor.
    await this.context.destroyActor(actor);
    ```


This call doesn't call the membership lifecycle callbacks again — it cleans up the native
actor ref, the Framework registry, and the bound session mapping. An Actor in a User Spot
can't be terminated directly. It has to finish leaving and return to the Entry Spot first.

## 4. User Spot Membership

A User Spot accepts or rejects a join request first. Once accepted and membership commits,
`OnJoinedActor` is called.

=== "C#/.NET"

    ```csharp
    public sealed class GameRoom(IZLinkSpotContext context)
        : IZLinkSpot<PlayerActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            var join = request.Decode<JoinGame>();
            return ValueTask.FromResult(
                HasSeat(join.Seat)
                    ? ZLinkSpotActorJoinResult.Accept(new Joined(join.Seat))
                    : ZLinkSpotActorJoinResult.Reject(new RoomFull()));
        }

        public ValueTask OnJoinedActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
            => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
            => ValueTask.CompletedTask;
    }
    ```

=== "C++"

    ```cpp
    class game_room_t : public spot_t<player_actor_t>
    {
      public:
        spot_context_t &context () noexcept override { return _context; }

        task_t<spot_actor_join_result_t>
        on_actor_join (std::string_view actor_id, const message_t &request) override
        {
            const auto join = request.decode<join_game_t> ();
            co_return has_seat (join.seat)
                     ? spot_actor_join_result_t::accept (joined_t{join.seat})
                     : spot_actor_join_result_t::reject (room_full_t{});
        }

        task_t<void> on_actor_joined (player_actor_t &) override { co_return; }
        task_t<void> on_leave_actor (player_actor_t &) override { co_return; }

      private:
        spot_context_t _context;
    };
    ```

=== "Java"

    ```java
    public final class GameRoom implements ZLinkSpot<PlayerActor> {
        private final ZLinkSpotContext context;

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
            JoinGame join = request.decode(JoinGame.class);
            return CompletableFuture.completedFuture(hasSeat(join.seat())
                ? ZLinkSpotActorJoinResult.accept(new Joined(join.seat()))
                : ZLinkSpotActorJoinResult.reject(new RoomFull()));
        }

        @Override
        public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    class GameRoom(private val spotContext: ZLinkSpotContext) : ZLinkSpot<PlayerActor> {

        override fun context(): ZLinkSpotContext = spotContext

        override suspend fun onActorJoin(
            actorId: String, request: ZLinkMessage): ZLinkSpotActorJoinResult {
            val join = request.decode(JoinGame::class.java)
            return if (hasSeat(join.seat)) ZLinkSpotActorJoinResult.accept(Joined(join.seat))
                   else ZLinkSpotActorJoinResult.reject(RoomFull())
        }

        override suspend fun onJoinedActor(actor: PlayerActor) {}
        override suspend fun onLeaveActor(actor: PlayerActor) {}
    }
    ```

=== "Node/TypeScript"

    ```typescript
    export class GameRoom implements ZLinkSpot<PlayerActor> {
      readonly context!: ZLinkSpotContext<PlayerActor>;

      async onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
        const join = request.decode<JoinGame>(Object as never);
        return this.hasSeat(join.seat)
          ? ZLinkSpotActorJoinResult.accept(joined(join.seat))
          : ZLinkSpotActorJoinResult.reject(roomFull());
      }

      async onJoinedActor(actor: PlayerActor): Promise<void> {}
      async onLeaveActor(actor: PlayerActor): Promise<void> {}
    }
    ```


## 5. When A Join Actually Runs

### What `Defer()` Does

`Defer()` **schedules a join on the current handler instead of running it now.** At the
call, the Framework fixes three things — an immutable snapshot of the join request, the
absolute deadline computed from `Timeout(...)`, and the barrier to run once this handler
finishes.

What happens to the scheduled barrier depends on how the handler ends.

| How the handler ends | The scheduled join |
| --- | --- |
| Ends normally | Activates and starts running |
| Exception, cancellation, or reply-encoding failure | Discarded. The join never starts |

`Defer()` can only be called **while the current handler's registration scope is open.**
Calling it after the handler finishes, or from a background task detached from the handler,
is `InvalidOperation`.

**Where it can be called is fixed.**

| Can call it | Can't call it |
| --- | --- |
| An Actor send/request handler | The factory and configuration phase |
| A packet/request/subscription/timer handler on a User/Entry Spot | A lifecycle callback |
| | A relocation adapter |
| | An Instance Spot handler |
| | A background task detached from a handler |

Calling it from the right column is `InvalidOperation`. **The Framework doesn't guarantee
catching a detached task in every language** — it might not be discovered before the handler
finishes, so simply don't call it from that spot in the first place.

Calling `Defer()` twice in the same call is `InvalidOperation`, and if that Actor already
has a different membership transition in flight, it's `Unavailable`. **If an Actor already
belonging to that Spot joins the same Spot again**, it ends in success without changing
location — it touches neither the Store nor membership, and doesn't run the
join/joined/leave callbacks either.

### Why `JoinSpot` Only Ever Runs Through `Defer()`

The join call has no `Async`. The reason it doesn't provide a form that waits for the result
right there is what a join actually does.

- **A join changes this Actor's location and membership.** If the target Spot's owner is a
  different node, it performs Actor relocation within the same operation — location lookup,
  the target admission callback, and the Store commit are all included.
- **Waiting for its completion within the current turn blocks itself.** An Actor executes
  its queue's jobs one at a time. If the currently executing handler waits for the join to
  complete, this Actor's follow-up work needed for that join to finish (the lifecycle
  callback after the membership commit) ends up waiting in the same queue.
- **The execution subject at completion time can change.** If a cross-node join succeeds,
  the one that receives the `Accepted` callback is **the target node's Actor.** The source
  Actor, where the current handler is, is already being cleaned up by that point, so the
  very shape of "receive the result inside this handler" doesn't hold.

So the contract **separates registration from execution.** The handler schedules the join
and ends normally, and the Framework starts location lookup and Store work after that. The
result arrives through the completion callback below. This separation is what keeps the
current Actor job's execution order from getting tangled with the join-completion callback's.

Once the barrier is activated, an ordinary message that arrives after it never runs ahead of
the completion callback. That Actor's ordinary processing waits until the join finishes.

### Registration And Receiving The Result

Schedule the join from an Actor handler. The handler is a separate class that receives a
one-way packet addressed to a member Actor
([06-spot §4.1](06-spot.ko.md#41-handler-종류와-구현할-interface)), registered as an actor
packet during the configuration phase. After `Defer()`, there's nothing left to do except
let this handler end normally.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the handler where a player schedules entering a room. Actual code from the repository.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayActorJoinGameHandler.cs:doc-join-defer"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play_actor_join_game_handler.hpp:doc-join-defer"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/entryspot/handlers/PlayActorJoinGameHandler.java:doc-join-defer"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/handlers/PlayActorJoinGameHandler.kt:doc-join-defer"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts:doc-join-defer"
    ```

In its minimal shape, it looks like this.

=== "C#/.NET"

    ```csharp
    public sealed class JoinGameHandler
        : IZLinkSpotActorSendHandler<PlayEntrySpot, PlayerActor, JoinGame>
    {
        public ValueTask HandleAsync(
            PlayEntrySpot entrySpot,        // The Spot this Actor currently belongs to.
            PlayerActor actor,              // The Actor requesting the join.
            IZLinkMessageContext messageContext,
            JoinGame command,
            CancellationToken cancellationToken)
        {
            actor.Context
                .JoinSpot(command.SpotId, new JoinGameRequest(command.Seat))
                .Timeout(TimeSpan.FromSeconds(5))
                .Defer(); // Starts the join once the current handler succeeds.

            return ValueTask.CompletedTask;
        }
    }
    ```

=== "C++"

    ```cpp
    // C++ registers a member function on the Entry Spot instead of a handler class.
    task_t<void> play_entry_spot_t::join_game (player_actor_t &actor,   // The Actor requesting the join.
                                               message_context_t &,
                                               const join_game_req_t &request)
    {
        actor.context ()
          .join_spot (request.spot_id, join_game_request_t{request.seat})
          .timeout (std::chrono::seconds (5))
          .defer (); // Starts the join once the current handler succeeds.
        co_return;
    }
    ```

=== "Java"

    ```java
    public final class JoinGameHandler
        implements ZLinkSpotActorSendHandler<PlayEntrySpot, PlayerActor, JoinGame> {

        @Override
        public CompletionStage<Void> handle(
            PlayEntrySpot entrySpot,  // The Spot this Actor currently belongs to.
            PlayerActor actor,        // The Actor requesting the join.
            ZLinkMessageContext messageContext,
            JoinGame command) {
            actor.context()
                .joinSpot(command.spotId(), new JoinGameRequest(command.seat()))
                .timeout(Duration.ofSeconds(5))
                .defer(); // Starts the join once the current handler succeeds.
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    class JoinGameHandler : ZLinkSpotActorSendHandler<PlayEntrySpot, PlayerActor, JoinGame> {

        override suspend fun handle(
            entrySpot: PlayEntrySpot,  // The Spot this Actor currently belongs to.
            actor: PlayerActor,        // The Actor requesting the join.
            messageContext: ZLinkMessageContext,
            command: JoinGame,
        ) {
            actor.context()
                .joinSpot(command.spotId, JoinGameRequest(command.seat))
                .timeout(Duration.ofSeconds(5))
                .defer() // Starts the join once the current handler succeeds.
        }
    }
    ```

=== "Node/TypeScript"

    ```typescript
    export class JoinGameHandler
      implements ZLinkSpotActorSendHandler<PlayEntrySpot, PlayerActor, JoinGame> {

      async handle(
        entrySpot: PlayEntrySpot,  // The Spot this Actor currently belongs to.
        actor: PlayerActor,        // The Actor requesting the join.
        messageContext: ZLinkMessageContext,
        command: JoinGame
      ): Promise<void> {
        actor.context
          .joinSpot(command.spotId, joinGameRequest(command.seat))
          .timeout(5_000)
          .defer(); // Starts the join once the current handler succeeds.
      }
    }
    ```


The result arrives through the Actor's `OnJoinCompleted`. Which Actor runs this callback
depends on the result — `Accepted` goes to the **target** Actor that committed the location
change, while `Rejected` and a pre-commit `Failed` go to the original **source** Actor.

=== "C#/.NET"

    ```csharp
    public ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        switch (completion)
        {
            // The location and membership change committed. accepted.Actor is the current ActorRef.
            case ZLinkActorJoinCompletion.Accepted accepted:
                RememberCurrentLocation(accepted.Actor);
                break;

            // The target's admission callback rejected the join. The location is unchanged.
            case ZLinkActorJoinCompletion.Rejected:
                ClearPendingJoin();
                break;

            // Only the error kind is received. Decide whether to retry by checking business state and idempotency.
            case ZLinkActorJoinCompletion.Failed failed:
                HandleJoinFailure(failed.Kind);
                break;
        }

        return ValueTask.CompletedTask;
    }
    ```

=== "C++"

    ```cpp
    task_t<void> on_join_completed (const actor_join_completion_t &completion) override
    {
        // The location and membership change committed. accepted->actor is the current ActorRef.
        if (const auto *accepted = std::get_if<actor_join_accepted_t> (&completion)) {
            remember_current_location (accepted->actor);
            co_return;
        }
        // The target's admission callback rejected the join. The location is unchanged.
        if (std::get_if<actor_join_rejected_t> (&completion)) {
            clear_pending_join ();
            co_return;
        }
        // Only the error kind is received. Decide whether to retry by checking business state and idempotency.
        if (const auto *failed = std::get_if<actor_join_failed_t> (&completion);
            failed != nullptr) {
            handle_join_failure (failed->error_kind);
        }
        co_return;
    }
    ```

=== "Java"

    ```java
    @Override
    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        // The location and membership change committed. accepted.actor() is the current ActorRef.
        if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
            rememberCurrentLocation(accepted.actor());
        // The target's admission callback rejected the join. The location is unchanged.
        } else if (completion instanceof ZLinkActorJoinCompletion.Rejected) {
            clearPendingJoin();
        // Only the error kind is received. Decide whether to retry by checking business state and idempotency.
        } else if (completion instanceof ZLinkActorJoinCompletion.Failed failed) {
            handleJoinFailure(failed.kind());
        }
        return CompletableFuture.completedFuture(null);
    }
    ```

=== "Kotlin"

    ```kotlin
    override suspend fun onJoinCompleted(completion: ZLinkActorJoinCompletion) {
        when {
            // The location and membership change committed. completion.actor() is the current ActorRef.
            completion is ZLinkActorJoinCompletion.Accepted ->
                rememberCurrentLocation(completion.actor())
            // The target's admission callback rejected the join. The location is unchanged.
            completion is ZLinkActorJoinCompletion.Rejected ->
                clearPendingJoin()
            // Only the error kind is received. Decide whether to retry by checking business state and idempotency.
            completion is ZLinkActorJoinCompletion.Failed ->
                handleJoinFailure(completion.kind())
        }
    }
    ```

=== "Node/TypeScript"

    ```typescript
    async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
      switch (completion.status) {
        // The location and membership change committed. completion.actor is the current ActorRef.
        case 'accepted':
          this.rememberCurrentLocation(completion.actor);
          break;
        // The target's admission callback rejected the join. The location is unchanged.
        case 'rejected':
          this.clearPendingJoin();
          break;
        // Only the error kind is received. Decide whether to retry by checking business state and idempotency.
        case 'failed':
          this.handleJoinFailure(completion.kind);
          break;
      }
    }
    ```


Going back from a User Spot to the Entry Spot works the same way.

=== "C#/.NET"

    ```csharp
    actor.Context
        .JoinEntrySpot(new LeaveGame(reason))
        .Timeout(TimeSpan.FromSeconds(5))
        .Defer();
    ```

=== "C++"

    ```cpp
    actor.context ()
      .join_entry_spot (leave_game_t{reason})
      .timeout (std::chrono::seconds (5))
      .defer ();
    ```

=== "Java"

    ```java
    actor.context()
        .joinEntrySpot(new LeaveGame(reason))
        .timeout(Duration.ofSeconds(5))
        .defer();
    ```

=== "Kotlin"

    ```kotlin
    actor.context()
        .joinEntrySpot(LeaveGame(reason))
        .timeout(Duration.ofSeconds(5))
        .defer()
    ```

=== "Node/TypeScript"

    ```typescript
    actor.context
      .joinEntrySpot(leaveGame(reason))
      .timeout(5_000)
      .defer();
    ```


`OperationId` is an idempotency ID that distinguishes whether this completion is the result
of a retry. Handle a callback for the same `OperationId` running again safely.

### Registration Limits

There's a ceiling on how much one handler can schedule.

| What | Ceiling |
| --- | --- |
| Number of joins one handler can schedule | 64 |
| Encoded size of one join request | 1 MiB |
| Sum of request sizes one handler has scheduled | 8 MiB |
| A cross-node join's application reply | 1 MiB |
| Default timeout | 5 seconds. If specified, must be a finite positive value |

**Exceeding the ceiling ends immediately in an error.** It never leaves a state where only
part of it registered and the rest was dropped. The request and reply ceilings are
independent and aren't computed as a combined total.

### Don't Send A Request To A Scheduled Actor

Sending a request **from the same handler** to an Actor that already has a `Defer()` barrier
attached, and waiting for the reply, creates a **circular wait.** The request waits behind
the barrier, the barrier only opens once this handler finishes, and the handler can't finish
because it's waiting for the reply.

The Framework rejects this request with `InvalidOperation` **before it's ever submitted.**
It ends in an error instead of hanging, so if you see this error, check whether the
scheduled target and the request's target are the same Actor.

### When A Scheduled Join Doesn't Survive

The schedule and its barrier **exist only in the current process's memory.** If the process
goes down before the join runs or is reflected in the Store, that schedule isn't replayed.
The Actor's location and membership stay exactly as they were — it never ends up half-moved.

If it overlaps with `Relocate` or `Shutdown`, **whichever settled first wins.** If the join
already took its spot, maintenance waits until the join finishes; if the relocation seal came
first, the join ends in `Unavailable`; if the shutdown seal came first, it ends in
`ShuttingDown`.

## 6. Actor Messaging

You can send a message by ActorId without knowing which Spot or node the Actor is on.

=== "C#/.NET"

    ```csharp
    await actorClient
        .SendToActor(playerId, new AwardExperience(10))
        .Async(cancellationToken);

    PlayerProfile profile = await actorClient
        .RequestToActor(playerId, new GetPlayerProfile())
        .Timeout(TimeSpan.FromSeconds(3))
        .Async<PlayerProfile>(cancellationToken);
    ```

=== "C++"

    ```cpp
    co_await actor_client.send_to_actor (player_id, award_experience_t{10}).submit ();

    auto profile = co_await actor_client
                     .request_to_actor (player_id, get_player_profile_t{})
                     .timeout (std::chrono::seconds (3))
                     .submit<player_profile_t> ();
    ```

=== "Java"

    ```java
    actorClient.sendToActor(playerId, new AwardExperience(10))
        .submit().toCompletableFuture().join();

    PlayerProfile profile = actorClient
        .requestToActor(playerId, new GetPlayerProfile())
        .timeout(Duration.ofSeconds(3))
        .submit(PlayerProfile.class)
        .toCompletableFuture().join();
    ```

=== "Kotlin"

    ```kotlin
    actorClient.sendToActor(playerId, AwardExperience(10)).submit().await()

    val profile = actorClient
        .requestToActor(playerId, GetPlayerProfile())
        .timeout(Duration.ofSeconds(3))
        .submit(PlayerProfile::class.java)
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    await actorClient.sendToActor(playerId, awardExperience(10)).submit();

    const profile = await actorClient
      .requestToActor(playerId, getPlayerProfile())
      .timeout(3_000)
      .submit<PlayerProfile>();
    ```


Even while an Actor is moving to another node, the caller specifies only the ActorId. The
Framework re-queries the **current owner** recorded in the Location Store on every call and
sends to that node.

A message a caller sends to the previous owner, because it had cached the location right
before the move, isn't dropped either. The previous owner node that received that message
**forwards it on the caller's behalf** to the new owner. This is called Message Follow — not
a redirect that tells the sender the new address and makes it resend, but a scheme where the
node that received it hands it off. This forwarding is valid only within the Message Follow
duration; a message that arrives after that is treated as an ordinary stale-route failure.
The application never tracks `NodeRid`.

**A request sent during the move also completes back at the original caller.** The reply
the target produced is correlated back to the original caller, the timeout follows the
caller's existing path as-is, and a reply that arrives late is dropped
([spot-actor spec §10.5](../../../common/spec/15-spot-actor.ko.md)). The number of requests
waiting on a reply during a move is observed through the `surface=actor` value of
`zlink.mesh_node.requests.inflight` ([12-operations](12-operations.ko.md#1-런타임-메트릭)).

## 7. Relocation State Adapter

The adapter saves and restores only the Actor instance's application state, as a byte array.
Location authority, queue, timer, the accepted journal, and the session route are all
handled by the Framework.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the adapter that packs and unpacks a player Actor's state. Actual code from the
> repository.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/PlayActorRelocationAdapter.cs:doc-relocation-adapter"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/player_actor_relocation_adapter.hpp:doc-relocation-adapter"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.java:doc-relocation-adapter"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.kt:doc-relocation-adapter"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Actors/play-actor-relocation-adapter.ts:doc-relocation-adapter"
    ```

In its minimal shape, it looks like this.

=== "C#/.NET"

    ```csharp
    public sealed class PlayerActorRelocationAdapter
        : IZLinkActorRelocationAdapter<PlayerActor>
    {
        public ValueTask<byte[]> CaptureAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
            => ValueTask.FromResult(actor.ExportState());

        public ValueTask RestoreAsync(
            PlayerActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            actor.ImportState(payload.Span);
            return ValueTask.CompletedTask;
        }
    }
    ```

=== "C++"

    ```cpp
    class player_actor_relocation_adapter_t final
        : public actor_relocation_adapter_t<player_actor_t>
    {
      public:
        task_t<std::vector<std::byte>> capture (player_actor_t &actor, std::stop_token) override
        {
            auto state = actor.export_state ();
            co_return std::vector<std::byte> (state.begin (), state.end ());
        }

        task_t<void> restore (player_actor_t &actor,
                              std::vector<std::byte> payload,
                              std::stop_token) override
        {
            actor.import_state (payload);
            co_return;
        }
    };
    ```

=== "Java"

    ```java
    public final class PlayerActorRelocationAdapter
        implements ZLinkActorRelocationAdapter<PlayerActor> {

        @Override
        public CompletionStage<byte[]> capture(PlayerActor actor) {
            return CompletableFuture.completedFuture(actor.exportState());
        }

        @Override
        public CompletionStage<Void> restore(PlayerActor actor, byte[] payload) {
            actor.importState(payload);
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    class PlayerActorRelocationAdapter : ZLinkActorRelocationAdapter<PlayerActor> {

        override suspend fun capture(actor: PlayerActor): ByteArray = actor.exportState()

        override suspend fun restore(actor: PlayerActor, payload: ByteArray) {
            actor.importState(payload)
        }
    }
    ```

=== "Node/TypeScript"

    ```typescript
    export class PlayerActorRelocationAdapter
      implements ZLinkActorRelocationAdapter<PlayerActor> {

      async capture(actor: PlayerActor): Promise<Uint8Array> {
        return actor.exportState();
      }

      async restore(actor: PlayerActor, payload: Uint8Array): Promise<void> {
        actor.importState(payload);
      }
    }
    ```


Capture and restore can be called again within the same relocation. The adapter must be
retry-safe, and must copy the payload memory if it's kept around outside the callback.

## 8. Related Documents

- Runnable verification examples for this chapter's contract: `13. Interface Catalog`
  chapter §4 — the verification class `ActorContracts`
- Session and Actor binding: [Session Actor Dispatch](08-actor-session.ko.md)
- The STREAM server and client: [STREAM](09-stream.ko.md)
- The Actor/Spot address resolution rule: [Object routing](../../../common/spec/18-object-routing.ko.md)
