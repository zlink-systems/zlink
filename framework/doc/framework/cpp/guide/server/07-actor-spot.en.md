---
title: "7. Actor And Spot · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/07-actor-spot.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 6. Spot](06-spot.en.md) | [Next: 8. Session And Actor Binding](08-actor-session.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/07-actor-spot.en.md) · **C++** · [Java](../../../java/guide/server/07-actor-spot.en.md) · [Kotlin](../../../kotlin/guide/server/07-actor-spot.en.md) · [Node/TypeScript](../../../node/guide/server/07-actor-spot.en.md)
<!-- language-switch:end -->

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

The relocation policy is fixed once, at factory registration, and doesn't change while
running. This policy applies both when an Actor joins another node's Spot and when it moves
via host `relocate`.

| Policy | How it's recreated on another node |
| --- | --- |
| `DisableRelocation()` | Refuses before a cross-node move even starts. If this target remains, host relocation can't complete. |
| `RecreateOnRelocation()` | Creates a new instance with the same logical identity. Pending messages and timers are preserved, but application state isn't restored. |
| `PreserveStateWith<TAdapter>()` | Restores the `byte[]` the adapter saved onto the new instance. The Framework queue and timers are preserved as well. |

## 2. Creating An Actor

`create` fails if the same ActorId already exists. `get_or_create` returns `Existing` if a
Ready Actor of the same type already exists. The caller never specifies the target node.

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

`ActorRef` carries the exact incarnation and the owner route as of the lookup. It's used for
session binding or exact destroy. Ordinary Actor messaging uses only the ActorId.

```cpp
auto current = co_await actors.find (player_id);
auto current_spot = co_await actors.find_spot (player_id);

if (current) {
    // Doesn't terminate an Actor whose generation differs.
    co_await actors.destroy (current.value ());
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
| `on_create_actor` | When a new Actor takes this Entry Spot as its first membership. Decides accept/reject |
| `OnJoinedActor` | Once the commit finishes for an Actor that was in another Spot coming into this Entry Spot |
| `on_leave_actor` | Once the commit finishes for an Actor that was in this Entry Spot leaving to another Spot |
| `on_disconnect_actor` | When the client connection for an Actor belonging to this Entry Spot drops |

These callbacks aren't called when an Actor is restored into another node's Entry Spot via
relocation. Relocation keeps membership exactly as it is and only moves the execution
location, so from the application's point of view it's not an event of "coming in" or
"going out."

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the Entry Spot a player first enters. Actual code from the repository.

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp:doc-entry-spot"
```

In its minimal shape, it looks like this.

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

It's safer for an Entry Spot not to keep per-Actor application state of its own. The Actor
owns its state; the Entry Spot only provides handlers and the membership lifecycle.

To terminate an Actor, first return it to the Entry Spot, then pass the current Actor
instance to the Entry Spot context's actor-destroy call.

```cpp
// The Entry Spot requests termination of the current Actor.
co_await _context.destroy_actor (actor);
```

This call doesn't call the membership lifecycle callbacks again — it cleans up the native
actor ref, the Framework registry, and the bound session mapping. An Actor in a User Spot
can't be terminated directly. It has to finish leaving and return to the Entry Spot first.

## 4. User Spot Membership

A User Spot accepts or rejects a join request first. Once accepted and membership commits,
`OnJoinedActor` is called.

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

## 5. When A Join Actually Runs

### What `defer()` Does

`defer()` **schedules a join on the current handler instead of running it now.** At the
call, the Framework fixes three things — an immutable snapshot of the join request, the
absolute deadline computed from `timeout(...)`, and the barrier to run once this handler
finishes.

What happens to the scheduled barrier depends on how the handler ends.

| How the handler ends | The scheduled join |
| --- | --- |
| Ends normally | Activates and starts running |
| Exception, cancellation, or reply-encoding failure | Discarded. The join never starts |

`defer()` can only be called **while the current handler's registration scope is open.**
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

Calling `defer()` twice in the same call is `InvalidOperation`, and if that Actor already
has a different membership transition in flight, it's `Unavailable`. **If an Actor already
belonging to that Spot joins the same Spot again**, it ends in success without changing
location — it touches neither the Store nor membership, and doesn't run the
join/joined/leave callbacks either.

### Why `join_spot` Only Ever Runs Through `defer()`

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
packet during the configuration phase. After `defer()`, there's nothing left to do except
let this handler end normally.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the handler where a player schedules entering a room. Actual code from the repository.

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play_actor_join_game_handler.hpp:doc-join-defer"
```

In its minimal shape, it looks like this.

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

The result arrives through the Actor's `on_join_completed`. Which Actor runs this callback
depends on the result — `Accepted` goes to the **target** Actor that committed the location
change, while `Rejected` and a pre-commit `Failed` go to the original **source** Actor.

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

Going back from a User Spot to the Entry Spot works the same way.

```cpp
actor.context ()
  .join_entry_spot (leave_game_t{reason})
  .timeout (std::chrono::seconds (5))
  .defer ();
```

`operation_id` is an idempotency ID that distinguishes whether this completion is the result
of a retry. Handle a callback for the same `operation_id` running again safely.

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

Sending a request **from the same handler** to an Actor that already has a `defer()` barrier
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

If it overlaps with `relocate` or `shutdown`, **whichever settled first wins.** If the join
already took its spot, maintenance waits until the join finishes; if the relocation seal came
first, the join ends in `Unavailable`; if the shutdown seal came first, it ends in
`ShuttingDown`.

## 6. Actor Messaging

You can send a message by ActorId without knowing which Spot or node the Actor is on.

```cpp
co_await actor_client.send_to_actor (player_id, award_experience_t{10}).submit ();

auto profile = co_await actor_client
                 .request_to_actor (player_id, get_player_profile_t{})
                 .timeout (std::chrono::seconds (3))
                 .submit<player_profile_t> ();
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

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/player_actor_relocation_adapter.hpp:doc-relocation-adapter"
```

In its minimal shape, it looks like this.

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

Capture and restore can be called again within the same relocation. The adapter must be
retry-safe, and must copy the payload memory if it's kept around outside the callback.

## 8. Related Documents

- Runnable verification examples for this chapter's contract: `13. Interface Catalog`
  chapter §4 — the verification class `ActorContracts`
- Session and Actor binding: [Session Actor Dispatch](08-actor-session.ko.md)
- The STREAM server and client: [STREAM](09-stream.ko.md)
- The Actor/Spot address resolution rule: [Object routing](../../../common/spec/18-object-routing.ko.md)
