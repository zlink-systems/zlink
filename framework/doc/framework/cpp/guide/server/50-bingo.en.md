---
title: "Reading Along: Bingo · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/50-bingo.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: Bingo

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 14. Picking a Sample — Start with the Example Closest to Your Problem](14-samples.en.md) | [Next: Reading Along: TicTacToe](51-tictactoe.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/server/50-bingo.en.md) · [Java](../../../java/guide/server/50-bingo.en.md) · [Kotlin](../../../kotlin/guide/server/50-bingo.en.md) · [Node/TypeScript](../../../node/guide/server/50-bingo.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the Bingo sample in an editor and follow a message from the client's
    authentication to the end of a game, through the code of each server it passes. The code in this
    chapter comes from the [Bingo sample in the per-language example repositories](https://github.com/zlink-systems/zlink-cpp-examples/tree/main/samples/Bingo).

[Picking a Sample](14-samples.en.md#3-bingo--building-an-online-game-server) introduced what this
sample demonstrates. This chapter is what you read after that introduction — the roles and where
their code lives, the message flow of the main scenarios, and, for each flow, the framework feature
it uses and the chapter that explains it, in the order the source is laid out. This chapter explains
the Bingo sample's roles and code locations, its main message flows, and its run verification in
source order. See the [Bingo scenario](../../../common/sample/bingo/README.en.md) for requirements,
message contracts, and verification criteria.

## 1. What This Sample Demonstrates

The client keeps a single STREAM connection to a Session server. API handles authentication,
Matchmaking reserves the waiting room, the Play server that owns the room runs the game, and the
Location Store resolves both the connections between servers and the location of every object. A
game proceeds as authentication → matching → room join → card submission → timer draws and pushes →
winner and reward publish → cleanup, and the sections of this chapter follow the same order.

<iframe class="zlink-diagram" src="/common/diagrams/14-bingo-en.html" title="Bingo sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-bingo-en.html" target="_blank">↗ View larger</a></p>

This layout uses more framework features than any other sample because each flow needs a different
one. Binding a player to a single connection uses session binding; reserving a waiting room uses an
Instance Spot that is created when the first request arrives; the room is a User Spot that processes
members, timers and pushes in one line; and telling observers on other Play servers uses Logical
Multicast.

## 2. Roles and Where the Code Lives

Each role runs as its own process, and its code sits under `Server/<Role>` with the same name. The
package path differs per language, but the directory names are the same.

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| Session | 2 | STREAM connections, packets before authentication, Actor binding and relay | `Server/Session` |
| API | 2 | Authentication, player records, matching coordination | `Server/Api` |
| Matchmaking | 1 | Waiting-room reservation per level bucket — Redis holds the decision | `Server/Matchmaking` |
| Play | 2 | Player Actors, room Spots, timers, pushes, reward publish | `Server/Play/Infrastructure` |
| Client | 1 | The scenario from authentication to the end of observation, with self-checks | `Client` |

The Bingo rules — card validation, number draws, marks and the winner decision — live in
`Server/Play/Domain` and reference no framework type. This chapter does not cover them. Message names
and fields are set by the Protobuf schema in `Shared`; this is the only sample whose payload is
Protobuf.

## 3. Server Configuration

Each role's host configuration shows what that role receives and what it calls. Session receives
STREAM connections, calls objects in the Play mesh, and calls the API channel.

`Server/Session/session_server_host_factory.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Session/session_server_host_factory.hpp:doc-bingo-session-register"
```

Registering the session type on the stream node makes packets flow to the Actor bound to an
authenticated session — the managed languages also enable actor dispatch there. The mesh and channel registrations carry no remote endpoint —
which node serves which name is resolved by the Location Store ([Location](25-location.en.md)).

Play registers the Entry Spot, the player Actor factory and the room Spot factory on the same mesh.

`Server/Play/play_server_host_factory.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/play_server_host_factory.hpp:doc-bingo-play-register"
```

The Entry Spot exists once per node and is where an Actor is first created. A room is a User Spot,
one per id, executed as `SpotWide`. Handlers are registered by scanning in the managed languages and
through the builder in C++ ([Handlers and Message Processing](31-handler-dispatch.en.md)). The
differences between the kinds are covered by
[Activation and Lifetime](34-activation-lifetime.en.md).

## 4. Authentication and the Session–Actor Binding

The first packet the client sends is an authentication request. Session has API verify the token,
creates or finds the player Actor by its global id, and binds that Actor to the current session.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-auth-binding-en.html" title="Authentication and binding" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-auth-binding-en.html" target="_blank">↗ View larger</a></p>

The session handler sends a request to the API channel and, if the result is valid, obtains the
Actor from the Play mesh. The Actor's creation payload travels with that call.

`Server/Session/Sessions/Handlers/authenticate_session_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Session/Sessions/Handlers/authenticate_session_handler.hpp:doc-bingo-session-auth"
```

The call that obtains the Actor does not decide which Play node creates it. It returns the existing
Actor if there is one and a newly created Actor otherwise, and either way Session binds the returned
reference as it is.

`Server/Session/Sessions/Handlers/authenticate_session_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Session/Sessions/Handlers/authenticate_session_handler.hpp:doc-bingo-session-bind"
```

Packets that arrive after the binding are received by the Actor's handlers, not by the session
handlers. The session's dispatch callback makes that split — a packet that no registered session
handler handles is relayed to the bound Actor.

`Server/Session/Sessions/bingo_session.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Session/Sessions/bingo_session.hpp:doc-bingo-session-relay"
```

Session stores no Play endpoint. When the Actor moves through relocation, the framework updates the
relay route. Binding itself is covered by [Session and Actor](24-actor-session.en.md); how
many may be bound and how the route is refreshed by
[How Session Binding Works](39-session-binding.en.md).

## 5. Matching and Preparing the Room

A matching request is relayed to the bound Actor and arrives at the Entry Spot's actor request
handler. The handler asks API for a match, reserves a room join with the `RoomId` it receives, and
replies.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-matching-start-en.html" title="Matching and game start" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-matching-start-en.html" target="_blank">↗ View larger</a></p>

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match_bingo_actor_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match_bingo_actor_handler.hpp:doc-bingo-match-actor"
```

The join does not run inside the handler; it runs after the handler returns. That is why the state in
the matching reply is the pre-join `WaitingForPlayers`, and the game start is confirmed by the push
that follows. The reservation rules are covered by
[Actor Membership](35-actor-membership.en.md#2-reserving-a-join--it-runs-after-the-handler-ends).

API asks the Matchmaker Instance Spot, whose id is the level bucket, for a reservation, then creates
or finds the room Spot with the `RoomId` it gets back.

`Server/Api/Handlers/match_bingo_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Api/Handlers/match_bingo_handler.hpp:doc-bingo-api-match"
```

An Instance Spot is created when its first request arrives. Concurrent requests for the same id wait
for the single creation, and the room's `get_or_create` follows the same rule — API does not decide
which Play node the room is created on. The Matchmaker decides reservations atomically in Redis, so it
keeps no recovery ground in process memory, and it closes itself with a timer when idle.

`Server/Matchmaking/matchmaking_server_host_factory.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Matchmaking/matchmaking_server_host_factory.hpp:doc-bingo-matchmaker-idle"
```

The next request after a close creates an Instance Spot of a new generation. When each kind is
created is covered by [Activation and Lifetime](34-activation-lifetime.en.md#1-the-kinds-of-spot),
and calling an Instance Spot by [Spot](21-spot.en.md#6-the-other-kinds-of-spot).

## 6. Room Join and Yield

When the reserved join runs, the room Spot's joined callback is invoked. The player branch has to
read the player record from API, and if it held the room's execution turn for that round trip, the
other player's join and the timer would wait. So the request is sent with `Yield`.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp:doc-bingo-room-join"
```

Why this request gives its turn back with `Yield` and checks the pending join again after resuming
is covered by [The Execution Model §5](32-execution-model.en.md#5-serial-execution-and-thread-occupancy).

## 7. Cards, the Draw Timer and Pushes

Once both players have submitted cards, the room's timer starts drawing. Each tick draws one number,
marks both cards, and pushes the updated state to both players.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-card-draw-en.html" title="Cards, draws and the winner decision" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-card-draw-en.html" target="_blank">↗ View larger</a></p>

The timer handler runs inside the room Spot's turn, so it never overlaps the card submission handler.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_room_draw_timer_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_room_draw_timer_handler.hpp:doc-bingo-draw-timer"
```

Pushes go to the session bound to each player Actor. Whichever Play node the Actor is on and
whichever Session node the session is on, the sender addresses only the Actor's bound session.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_room_draw_timer_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_room_draw_timer_handler.hpp:doc-bingo-bound-push"
```

Timer registration and where a tick executes are covered by
[Timers and Workers](36-timer-worker.en.md#1-timers--periodic-execution), and the path to a bound
session by [How Session Binding Works](39-session-binding.en.md).

## 8. Rewards over Logical Multicast

Once the winner is decided, the room sends the end-of-game pushes and then publishes the reward event.
An observer may be on a different Play node, so the room does not address a particular Spot; it
publishes with a topic that sets the scope — the managed languages name the channel as well.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-reward-observe-en.html" title="Reward observation" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-reward-observe-en.html" target="_blank">↗ View larger</a></p>

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp:doc-bingo-reward-publish"
```

The observer room on each Play node subscribes to the same topic. The subscription handler sends a
notify to the observer Actor's bound session only when the event's `RoomId` matches the room it
observes.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_reward_acquired_event_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo_reward_acquired_event_handler.hpp:doc-bingo-reward-subscribe"
```

A publish that completes normally means the publish was started, not that a subscriber processed it.
The client confirms delivery by the notify that reaches the observer, not by the publish result. The
forms of pub/sub and target selection are covered by
[How Channels Work](30-channel-patterns.en.md#5-the-forms-of-pubsub).

## 9. End-of-Game Cleanup and Disconnect

When the game ends, the room removes the player Actors. Each Actor returns to the Entry Spot, and
the Entry Spot checks the destroy mark and destroys the Actor. The room does not destroy the Actor
itself because its leave callback gives the turn back with `Yield` while it reports the result to API.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-end-cleanup-en.html" title="End-of-game cleanup" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-end-cleanup-en.html" target="_blank">↗ View larger</a></p>

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp:doc-bingo-room-cleanup"
```

The Entry Spot's joined callback checks the mark and calls destroy. Destroy cleans up the Actor
object, the registry and the session binding, and does not invoke the leave callback again.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp:doc-bingo-entry-destroy"
```

A dropped client connection is a separate path. When the connection drops, the framework notifies
every Actor bound at that moment. The session's disconnect callback only logs that fact; it neither
destroys the Actor nor removes it from the room — some languages also call the notification
directly, but even when it overlaps the automatic one the callback runs once.

`Server/Session/Sessions/bingo_session.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Session/Sessions/bingo_session.hpp:doc-bingo-session-disconnect"
```

The order in which an Actor returns to the Entry Spot is covered by
[Activation and Lifetime](34-activation-lifetime.en.md), and where the disconnect notification lands
by [How Session Binding Works](39-session-binding.en.md#3-notification-when-the-connection-drops).

## 10. Preparing for Relocation

The room Spot factory chooses `SpotWide` execution and application-signaled relocation timing, and
names the adapter that preserves state. The Node implementation disables relocation for the room Spot
and keeps an adapter only for the player Actor.

`Server/Play/play_server_host_factory.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/play_server_host_factory.hpp:doc-execution-mode"
```

The adapter serializes only domain state. Queues, timers, the accepted journal and the owner fence
are moved by the framework and are not put in the adapter payload.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_relocation_adapter.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_relocation_adapter.hpp:doc-bingo-relocation-adapter"
```

In the implementations with relocation enabled, the room reserves relocation-ready in the turn where
a round's pushes and publish have finished — that call sits at the end of the timer handler shown
above. The unit of a move and the point of no return
are covered by [Relocation](37-relocation.en.md).

## 11. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

```bash
framework/languages/cpp/samples/Bingo/run_sample.sh
```

The client asserts the authentication results, the matching state, the push order and the reward
notify, and prints `bingo=completed` at the end. The runner counts the lifecycle lines in the server
logs and prints `bingo-placement=completed`. The checks and the exact log strings are set by the
[Bingo scenario](../../../common/sample/bingo/README.en.md#9-client-self-check).

## 12. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [Bingo scenario](../../../common/sample/bingo/README.en.md)
- The same game built with manual connections and manual registration:
  [Reading Along: TicTacToe](51-tictactoe.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
