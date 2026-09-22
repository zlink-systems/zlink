---
title: "Reading Along: TicTacToe · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/51-tictactoe.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: TicTacToe

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Reading Along: Bingo](50-bingo.en.md) | [Next: Reading Along: SupportChat](52-supportchat.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/server/51-tictactoe.en.md) · [Java](../../../java/guide/server/51-tictactoe.en.md) · [Kotlin](../../../kotlin/guide/server/51-tictactoe.en.md) · [Node/TypeScript](../../../node/guide/server/51-tictactoe.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the TicTacToe sample in an editor and follow the code from the HTTP call that
    creates a room to the point where both players leave and their Actors are destroyed. The code in
    this chapter comes from the [TicTacToe sample in the per-language example repositories](https://github.com/zlink-systems/zlink-cpp-examples/tree/main/samples/TicTacToe).

[Picking a Sample](14-samples.en.md#3-tictactoe--building-a-real-time-head-to-head-game-server)
introduced what this sample demonstrates. This chapter is what you read after that introduction —
the roles and where their code lives, the message flow of the main scenarios, and, for each flow, the
framework feature it uses and the chapter that explains it, in the order the source is laid out.
This chapter explains the TicTacToe sample's roles and code locations, its main message flows, and
its run verification in source order. See the [TicTacToe scenario](../../../common/sample/tictactoe/README.en.md)
for requirements, message contracts, and verification criteria.

## 1. What This Sample Demonstrates

There is no separate Session server. `Play` owns the STREAM connection, the player Actor, the Entry
Spot and the room Spot together, and `Api` handles only HTTP room creation and authentication. The
connections between servers are not resolved through the Location Store; the code connects directly
to the endpoints the runner supplies. Handlers are not left to scanning either — each one is listed
in registration code.

<iframe class="zlink-diagram" src="/common/diagrams/14-tictactoe-en.html" title="TicTacToe sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-tictactoe-en.html" target="_blank">↗ View larger</a></p>

What is manual is the **connection between nodes**. Where a room or an Actor currently lives is still
resolved by the Location Store. The host connects to Play A and the guest and observer to Play B, so
within one game the node that owns the room and the node where an Actor was created are bound to
differ — how the framework handles that case is the second subject of this sample.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| Api | 2 | HTTP room creation, token verification | `Server/Api` |
| Play | 2 | STREAM session, Entry Spot, player Actor, room Spot, milestone publish | `Server/Play/Infrastructure` |
| Client | 3 | The host, guest and observer scenarios with self-checks | `Client` |

The board, the turn and the win/draw decision live in `Server/Play/Domain` and reference no framework
type. Messages are set by the JSON contract in `Shared`.

## 3. Server Configuration — Manual Connection and Manual Registration

Play registers the stream node, the API channel client and the route mesh, and adds the Entry Spot,
the player Actor factory and the room Spot factory to the mesh's object server. The remote endpoints
of the API channel and the mesh are read from settings and connected directly.

`Server/Play/play_server_host_factory.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/play_server_host_factory.hpp:doc-ttt-play-register"
```

The player Actor factory names an adapter that preserves state, and the room Spot factory disables
relocation. The Api side connects to Play's mesh endpoint the same way.

`Server/Api/api_server_host_factory.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Api/api_server_host_factory.hpp:doc-manual-peer-connect"
```

A manual connection still names the routing id — the framework matches the endpoint against the
Location Store descriptor to confirm the peer is the node it expects. The difference between
connection and discovery is covered by
[How Channels Work](30-channel-patterns.en.md#6-connection-and-discovery).

Handlers are not left to scanning; they are registered directly, with their packet names, on the
Spot and session handler registries — the host configuration above and each Spot's configure
callback hold those registrations. TicTacToe is the only sample that registers this way in the
managed languages. The registration
variants are covered by
[Handlers and Message Processing](31-handler-dispatch.en.md#1-variations-on-handler-registration).

## 4. Room Creation — Spot Create from an HTTP Request

The client creates a room over HTTP. Api's HTTP handler asks the Spot manager to create a room Spot
and replies with the `RoomId` the framework issued and the list of Play endpoints.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-create-auth-en.html" title="Room creation, authentication and join" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-create-auth-en.html" target="_blank">↗ View larger</a></p>

`Server/Api/Handlers/create_game_http_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Api/Handlers/create_game_http_handler.hpp:doc-create"
```

The Play endpoints in the reply are where the client connects, not the room's owner. Api does not
decide which Play the room is created on; where it was created is recorded in the Location Store.
Creating a User Spot is covered by [Spot](21-spot.en.md#4-the-calling-side--the-node-that-calls-a-spot),
and when it is created and which callbacks run by
[Activation and Lifetime](34-activation-lifetime.en.md#4-user-spot--created-by-the-application).

## 5. Authentication and Binding — Play Owns the Session Too

The client connects over STREAM to the Play endpoint it chose from the reply and authenticates. The
session handler has Api verify the token, then creates or finds the player Actor and binds it to the
current session.

`Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate_play_session_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate_play_session_handler.hpp:doc-ttt-session-bind"
```

Unlike Bingo, the server that accepts the connection and the server that owns the Actor are the same
process. Even so, the call that obtains the Actor does not decide which node creates it. Binding is
covered by [Session and Actor](24-actor-session.en.md), and the rule that a reconnected session binds
to the same Actor again by [How Session Binding Works](39-session-binding.en.md).

## 6. Room Join — Reservation and the Move to Another Node

The join request arrives at the Entry Spot's actor send handler. The handler only reserves the room
join and returns.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play_actor_join_game_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play_actor_join_game_handler.hpp:doc-join-defer"
```

When the reserved join runs and the node holding the Actor differs from the node that owns the room,
the framework moves the Actor to the room's owner inside the join operation. During the move the
application state is serialized by the adapter named on the player Actor factory.

`Server/Play/Infrastructure/ZLink/Actors/player_actor_relocation_adapter.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/player_actor_relocation_adapter.hpp:doc-ttt-actor-capture"
```

When both are on the same node, the adapter is not called. Once the join completes, the room Spot's
joined callback registers the participant and notifies the other participants.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp:doc-ttt-game-join"
```

The reservation rules are covered by
[Actor Membership](35-actor-membership.en.md#2-reserving-a-join--it-runs-after-the-handler-ends), and
what survives when an Actor is moved and the adapter's part by [Relocation](37-relocation.en.md).

## 7. Placing Marks and Pushes

A place-mark request is relayed to the bound Actor and arrives at the room Spot's actor request
handler. The handler applies the mark to the domain and replies with the updated state.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-place-mark-en.html" title="Placing marks and the final state" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-place-mark-en.html" target="_blank">↗ View larger</a></p>

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_place_mark_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_place_mark_handler.hpp:doc-actor-packet-handler"
```

The requester receives the state in the reply; the opponent receives the same state as a bound
session push.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Notifications/game_notification_publisher.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Notifications/game_notification_publisher.hpp:doc-ttt-broadcast"
```

The turn time limit is checked by the room Spot's timer. The timer is registered in the initialize
callback, and each tick runs inside the room's turn.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp:doc-ttt-timer-register"
```

A wrong turn or an already-used cell is an application rejection and ends in a typed error reply.
The handler kinds are covered by
[Handlers and Message Processing](31-handler-dispatch.en.md#4-the-handler-kinds-of-spots-and-actors),
and timers by [Timers and Workers](36-timer-worker.en.md#1-timers--periodic-execution).

## 8. Milestones over Logical Multicast

When the host's accumulated wins reach 100, the room Spot publishes a milestone event. The observer is
connected to a different Play than the host, so the room publishes with a channel and a topic and
never needs to know where the observer is.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-milestone-en.html" title="Wins 100 milestone" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-milestone-en.html" target="_blank">↗ View larger</a></p>

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp:doc-multicast-publish"
```

The Entry Spot on each Play subscribes to the same topic. The subscription handler is registered on
the same registry as the packet handlers.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp:doc-multicast-subscribe"
```

The Entry Spot that receives the event sends a notify to every Actor that asked to observe, and each
Actor's bound session carries it to the client. The observer is not a member of the room, and there is no separate Spot type for it.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/player_win_milestone_event_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/player_win_milestone_event_handler.hpp:doc-ttt-milestone-notify"
```

A publish that completes normally does not mean a subscriber has processed it. The client confirms
delivery by the notify that reaches the observer. The forms of pub/sub are covered by
[How Channels Work](30-channel-patterns.en.md#5-the-forms-of-pubsub).

## 9. Disconnect, Reconnect and Destroy

When the connection drops, the framework notifies the bound Actor. The Actor is not deleted and the
room membership does not change. When the client reconnects and authenticates, a new session is
bound to the Actor with the same id, and the client sends the join again with the same `RoomId` to
receive the current state.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-disconnect-en.html" title="Disconnect and destroy" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-disconnect-en.html" target="_blank">↗ View larger</a></p>

After the game ends, each player sends a leave; the room Spot marks that Actor for destruction and
removes it from the room.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_leave_game_handler.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play_actor_leave_game_handler.hpp:doc-ttt-leave-game"
```

The Actor returns to the Entry Spot, and the Entry Spot's joined callback checks the mark and calls
destroy.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp`

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp:doc-ttt-entry-destroy"
```

The disconnect notification is covered by
[How Session Binding Works](39-session-binding.en.md#3-notification-when-the-connection-drops), and
the return to the Entry Spot and destroy by
[Activation and Lifetime](34-activation-lifetime.en.md#3-entry-spot--created-by-the-framework).

## 10. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

```bash
framework/languages/cpp/samples/TicTacToe/run_sample.sh
```

The client asserts the room creation reply, authentication, the join notify, the place-mark results,
the milestone notify and the state after reconnecting. The runner checks the server logs for each
Actor's room leave and Entry Spot destroy. The checks and the exact log strings are set by the
[TicTacToe scenario](../../../common/sample/tictactoe/README.en.md#9-client-self-check).

## 11. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [TicTacToe scenario](../../../common/sample/tictactoe/README.en.md)
- The same game built with automatic connection, automatic registration and a separate Session
  server: [Reading Along: Bingo](50-bingo.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
