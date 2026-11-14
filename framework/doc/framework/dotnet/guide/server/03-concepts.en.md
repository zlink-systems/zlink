---
title: "3. Core Concepts · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/03-concepts.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# 3. Core Concepts

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: .NET Quickstart — from Install to a First Request](../../quickstart.en.md) | [Next: Channel Messaging](20-channel-messaging.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/03-concepts.en.md) · **C#/.NET** · [Java](../../../java/guide/server/03-concepts.en.md) · [Kotlin](../../../kotlin/guide/server/03-concepts.en.md) · [Node/TypeScript](../../../node/guide/server/03-concepts.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can tell what a channel, a Spot, an Actor, STREAM and Location each are, and when to
    choose them.
    The code in this chapter comes from the [language-specific example repositories](https://github.com/zlink-systems/zlink-dotnet-examples).

The ZLink framework provides **channel · spot · actor · stream · location** as its core
concepts. Every other chapter is a variation on these. We'll go through them in order below,
and cover [relocation](#5-relocation--moving-to-another-node) — an actor or spot moving to
another node partway through — along the way. Later dedicated chapters cover how to implement
and operate each concept in practice.

## 1. channel — a connection between servers

**MeshNode** is the basic unit of a connection between servers. A single MeshNode can take
on two independent roles.

- **Object role** — the place where spots and actors are placed. Covered separately in
  [spot](#2-spot--a-unit-that-owns-state-and-processes-it-in-order) and
  [actor](#3-actor--a-state-object-identified-by-id).
- **Channel role** — the place where request/send/publish are exchanged. Covered in this
  document.

`ChannelName` is a logical name that groups together the nodes in that mesh sharing the same
function — instead of an address (`host:port`), you pick a call target by a name like
`"orders"`. The caller passes only the `ChannelName` to the route client — `MeshName` is
fixed at registration and never appears in the call arguments.

The caller doesn't need to know which node is currently handling that request. Pass only a
logical name (`ChannelName`, a spot id, or an actor id) — not an address or node number —
and the framework finds the target and delivers the request wherever that name currently
lives. This property
— **the caller doesn't need to know where the target is** — is called location
transparency. Channels, spots, and actors all work this way. That's why the calling code stays
the same whether you scale servers out or in.

When a message is sent by `ChannelName`, the framework selects one of the nodes currently
able to receive the request and delivers it there — this selection is called
**select-one**.

<iframe class="zlink-diagram" src="/common/diagrams/03-channel-select-en.html" title="channel — calling by name (select-one)" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-channel-select-en.html" target="_blank">↗ View larger</a></p>

If three nodes own the same `orders` channel, one of them is selected per call. The caller
doesn't know — and doesn't need to know — which node was selected.

Here's what it looks like to configure both roles on one MeshNode.

The blocks below are the tutorial source as-is — mesh registration and the Object role come
from the server side, and the channel's call-only (client) role comes from the client side, so
the mesh and channel names are the tutorial's own `"game"`/`"profile"`, not `"orders"`/`"billing"`.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:object-server"
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-client-register"
```

Automatic connection management, which avoids hard-coding peer addresses and tracks servers
as they scale up or down, is covered by [Location](25-location.en.md).

> **Note:** `MeshName` and `ChannelName` are different names. You can register several
> `ChannelName`s on one mesh, and the same `ChannelName` can be used across different
> meshes.

Several registration types use the name "channel"; they differ in whether they share a
socket.

| Kind | Socket |
| --- | --- |
| route mesh channel | Shares the already-open MeshNode socket |
| ClientServer channel | Opens its own socket, separate from the MeshNode |
| fanout channel | Opens its own dedicated PUB/SUB socket |

pub/sub also splits two ways. **Logical Multicast**, exchanged between Spots over a route
mesh channel, uses the mesh socket as-is, and a **fanout channel** delivers over its own
socket to the connected subscribers with a matching topic subscription. The structural comparison of the three and how to
use them is covered by
[Channel Messaging](20-channel-messaging.en.md#2-the-three-channel-messaging-arrangements).

## 2. spot — a unit that owns state and processes it in order

Many domains — a game room, a guild, or an auction item — have **state that several requests
may access at the same time.**
Building this yourself means both finding the process that currently holds the state and
routing requests there, while also preventing concurrent access by incoming requests. Keep
the state in process memory and you have to manage that routing yourself; keep it in a DB or
Redis and you have to read, write, and lock on every request.

A spot lets the framework handle both concerns. It keeps the target as **a single in-memory
object** and processes incoming requests **serially, one at a time.**
Since two requests can never touch the same state at the same time, no lock is needed.

Unlike a channel, a spot is addressed by id. Send to the `"orders"` channel and any node
capable of doing that work handles it. Send a request to a spot id like `"room-42"`,
though, and the node where that spot is located receives the message and hands it to that
spot to process. The framework finds that node through the same location transparency
[seen earlier](#1-channel--a-connection-between-servers).

<iframe class="zlink-diagram" src="/common/diagrams/03-spot-queue-en.html" title="spot — owns state, processes in order" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-spot-queue-en.html" target="_blank">↗ View larger</a></p>

A spot is registered on the MeshNode's **Object role**. It's a separate surface from the same
MeshNode's Channel role.

Spots fall into three kinds — **Entry Spot · User Spot · Instance Spot** — based on when
they're created, and **execution mode** determines which work runs concurrently. The
differences between the three kinds, choosing an execution mode, and
registration/lifecycle/timer/outbound are covered by [Spot](21-spot.en.md).

## 3. actor — a state object identified by ID

An actor is a **stateful object identified by an ID.** A message that arrives with the same
ID is always handled by the same instance. An actor always belongs to some spot, and how it
binds to an external client connection continues in the
[next section](#4-stream--external-client-connections).

<iframe class="zlink-diagram" src="/common/diagrams/03-actor-route-en.html" title="actor — identified by id" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-actor-route-en.html" target="_blank">↗ View larger</a></p>

See [Actor](22-actor.en.md) and [Actor Membership](35-actor-membership.en.md) for details.

## 4. stream — external client connections

A stream is a **connection-oriented, bidirectional channel to an external client** such as
a mobile app or game client. Unlike a server-to-server
[channel](#1-channel--a-connection-between-servers), the server manages connection lifecycle
and heartbeats, and one connection maps to a server-side **session** object. Reconnecting
after a disconnect is the client connector's job.

When you **bind** a session to an [actor](#3-actor--a-state-object-identified-by-id), the
session stops handling messages that arrive over that connection itself and relays them to
the bound actor instead. The reverse direction works the same way — a push from the actor
goes out to the client through the session bound to that actor.

<iframe class="zlink-diagram" src="/common/diagrams/03-stream-en.html" title="stream — external client connection" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-stream-en.html" target="_blank">↗ View larger</a></p>

So **the node that accepts the connection and the node that runs domain logic can be split.**
Even if the session lives on a gateway node and the actor lives on a different node, the
framework keeps the relay path alive. Even if the actor moves via
[relocation](#5-relocation--moving-to-another-node), the same session remains connected to
the actor at its new location.

See [STREAM](23-stream.en.md) for details; how to bind a session to an actor is covered by
[Session and Actor](24-actor-session.en.md).

## 5. relocation — moving to another node

Relocation is when an actor or spot leaves its current owner node and keeps running on
another node. It starts from two distinct triggers.

An actor belongs to a spot, and a spot belongs to a node. Relocation preserves this
relationship exactly as it is — only the node it runs on changes.

**When an actor joins a spot on another node.** If an actor requests to join a User Spot
that lives on a different node, the moment the join is accepted the actor moves to that node
carrying its state and pending work along with it. This is a move the application triggers by
request.

<iframe class="zlink-diagram" src="/common/diagrams/03-relocation-en.html" title="relocation — an actor joins a spot on another node" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-relocation-en.html" target="_blank">↗ View larger</a></p>

The only thing the join call specifies is the **spot id `room-42`** — there's no argument
that names a target node. The framework looks up which node currently holds that spot in the
location store and moves actor P to that node. So the names "node A" and "node B" never
appear anywhere in the application code. Once the move finishes, actor P belongs to node B's
`room-42` spot and follows the same execution rules as Q and R.

**When an operator relocates a host for maintenance or deployment without downtime.** The
operator moves the spots and actors on one host to another host. The framework handles this
without individual join requests from the application, and once it's done, the original host
can be shut down.

<iframe class="zlink-diagram" src="/common/diagrams/03-host-relocate-en.html" title="host relocate — moving spot and actors as a whole" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-host-relocate-en.html" target="_blank">↗ View larger</a></p>

A server holding state can't simply be taken down, so maintenance or deployment usually
means dropping the connection and making clients wait. Host Relocate preserves the spot's
and actor's state while moving them to another node, freeing up node A. The caller still
sends requests to the same id, `room-42`, so it never needs to know the move happened. The
net effect is that **a stateful service can be replaced with zero downtime, the way a
stateless service would be.**

Both paths follow **the same relocation policy.** What happens to application state during
the move (not moved, recreated, or restored as-is) is fixed at spot/actor factory
registration and doesn't change while running.

Policy types and selection criteria are covered by
[Relocation](37-relocation.en.md#21-the-relocation-policy-the-factory-registration-picks); making an actor join call and receiving the
completion result are covered by [Actor Membership](35-actor-membership.en.md#23-where-the-result-arrives); and Host Relocate
as zero-downtime maintenance/deployment, along with how a relocation unit is scoped, is
covered by [12-operations §2](12-operations.en.md).

## 6. location — address resolution

Application code uses only logical names like a channel name, and the actual peer address
(`host:port`) is resolved by a **location store** shared across the whole deployment. Each
server registers its own location as a descriptor in the store on startup, and the calling
side looks up the target in the store by logical name and connects. When the server topology
changes, connections update accordingly.

Usage is covered by [Location](25-location.en.md).

Manual connections — specifying an endpoint directly at registration without a store — are
also supported, for development/testing and small fixed deployments
([How Channels Work](20-channel-messaging.en.md)). You can't use both approaches
together on the same MeshNode.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the smallest example where all of these concepts show up in one sample. They all meet
> in one place: the Play server's registration code.
>
> | Concept | In TicTacToe |
> | --- | --- |
> | channel | The Play server looks up credentials over an independent `tictactoe.api` ClientServer Channel |
> | spot | One match is one `TicTacToeGame` spot — both players' moves are processed serially inside it |
> | actor | A player is an actor, and even after a reconnect the same actor picks up the ongoing match |
> | stream | The client connects directly to the Play STREAM endpoint from the API response to make moves and receive pushes |
> | location | The Redis location store automatically picks which Play node will create a new `TicTacToeGame` spot — the API code has no specific Play node address |
>
> You've seen what problem each concept solves above; this sample shows **what it looks like
> when they're put together.**

## 7. Where to Start for What You're Trying to Do

Once the concepts are clear, the next question is, "So which surface do I use?" There are
**eight starting points**, all obtained from DI or the current handler context.
**The application never picks a transport socket or endpoint directly.**

| What you're trying to do | Starting surface | What you specify |
| --- | --- | --- |
| Send directly to a node, or by channel name | route client | Node-direct is MeshName + target RID; channel is ChannelName |
| Send to a Spot | spot client | Global Spot ID |
| Send to an Actor | actor client | Global Actor ID |
| Create or find a User Spot | spot manager | Spot type, and a global Spot ID if needed |
| Create or find an Actor | actor manager | Global Actor ID and Actor type |
| Publish a Logical Multicast | spot publisher client | ChannelName and topic |
| Publish classic pub/sub | fanout client | fanout ChannelName, and topic if needed |
| Send to or reply to a STREAM client | session client | The current session |

The exact type names are language-specific and are defined in the `13. Interface Catalog`
chapter.

**Completion has two consistent forms.** A send-family call finishes with
no return value once **the send slot accepts it**, and a request-family call finishes with
one of **reply · timeout · route error**. This holds no matter which surface you use
([Backpressure Visible in the API](33-backpressure.en.md#4-backpressure-visible-in-the-api)).

## 8. Naming Convention

The guide uses the following notation consistently throughout.

- **Every public type the framework adapter exposes** (interface, record, enum,
  attribute, exception, DI extension method) uses the `ZLink` prefix (capital `L`). e.g.
  `IZLinkRouteClient`, `IZLinkMessageContext`, `[ZLinkRequest]`, `AddZLinkFramework`,
  `ZLinkFrameworkException`.
- **Except**, types in the **client-side Stream Connector package**
  (`Zlink.Stream.Connector`) use the `Zlink` prefix (lowercase `l`). e.g.
  `IZlinkStreamConnector`, `ZlinkStreamConnectorOptions`, `ZlinkStreamMessage`. This is
  because the connector is an independent client library with no dependency on the
  server framework package.
- **The underlying zlink core C API** is `zlink_*` snake_case.
- NuGet package ids use `Zlink` or `Zlink.*`. Namespace and assembly names retain
  their established values and are independent from the package id.

> In short: **server framework = `ZLink`, client connector = `Zlink`.** If you see both
> notations in the same code, it's not a typo — it's this rule.

## 9. What the Framework Owns and What It Doesn't

**Everything below is handled internally by the framework.** None of it appears in
application code.

| Owned |
| --- |
| Transport address selection and peer reconnect |
| Multipart framing and packet codec |
| Reply correlation |
| Backpressure queue |

**These, by contrast, are outside this framework's contract scope.** They're handled by the
external edge.

| Not owned |
| --- |
| External client authentication and quota |
| WAF |
| Public API versioning |
| Billing |

Server-to-server communication and a real-time state server are this framework's territory.
Policy for an edge exposed directly to the internet is owned by whatever sits in front of it.

## 10. Stateful Game Nodes and the HTTP Lobby

A game server built with ZLink contains two kinds of code with different natures. Before writing code, decide which kind you are writing. Most mistakes AI tools make come from building game node code like a web server.

| | Game node | HTTP lobby |
|---|---|---|
| What it does | Holds room, round and player state and processes messages in order | Short requests and responses such as login, matchmaking and room assignment |
| State | In memory. Its owner is one Spot or Actor | None (stateless) |
| Database | Read on connect, write only results | May read and write per request |
| Template examples | The Room, Table, Field and Channel nodes of every template | The Lobby of `match-http-lobby` and `fps-http-lobby` |
| Do not | A REST-style structure that reads the database per request | Keep game state (round progress, balance changes) in the lobby |

The HTTP lobby may be built like an ordinary web server. From the moment it takes over game state, however, it follows the game node rules. The lobby only sends requests to game nodes; it never changes a game node's values directly.

The "common mistake" columns below come from code that AI tools wrote during real game development.

### 10.1 A Game Node Is Not a REST Server

| Rule | Common mistake |
|---|---|
| A player's state is owned by one player Actor bound to the session. Read it once on connect and keep it in memory; do not read the database while handling a request | Every slot spin re-read balance, missions and VIP from the database and wrote about seven small transactions. On an 8 vCPU server only two cores were busy, yet 1,800 players saturated it at p95 1.3 s. Every request SELECTed the account and sanctions, and every broadcast SELECTed each recipient's balance |
| Write only results to the database. Write the result of one round (one transaction) once, in one transaction | Each move waited for a resend-check SELECT and a record UPDATE before replying: about five database round trips per request |
| Check for duplicate requests in Spot or Actor memory (recent request IDs) as well | Same as above |
| A value many players see (ticker, jackpot news) is held by one node-memory copy or the one Spot that owns it, and refreshed periodically | Each player read the database every few seconds: about 400 queries per second at 1,800 players |
| When many requests update the same row, the one Spot that owns the value batches them | Every round UPDATEd a single jackpot-pool row and queued on its row lock |
| A process that does not own a value never writes it; it sends a request to the owner Actor | An operations process updated wallet rows directly, so the game service could not trust its in-memory values |
| While a game is running, a Spot handler never awaits a remote request or a database reply. An await inside a handler holds the turn, so every message and timer of that Spot stops | (design rule) |
| Do not write round results per Actor. When a round ends, the Spot writes the results of every Actor in that round in one transaction, then tells each Actor after the commit so their memory matches | (design rule) |

"The one Spot that owns the value" means a SpotWide Spot that holds state. A `PerActor` Spot keeps no state, so values several Actors change together go to an external store as [Execution model §4](32-execution-model.en.md#4-peractor-one-lane-each) describes, protected by conditional updates.

AI tools build it this way because common examples (Spring, NestJS and the like) are stateless REST: "load from repository → modify → save". When you see that shape in game node code, stop and move it to Actor and Spot ownership.

Exceptions and cautions for round settlement:

- The settlement unit is a round, not a whole game. A long game that writes only at its end loses all progress if the process dies. For slots, one spin is one round.
- Each game decides whether a player who leaves mid-round is settled at the end of the round or on leaving, without overlapping the next round or a move to another game.
- Money that must be final at that moment, such as buy-ins and in-game purchases, is not deferred to the end of the round. The player Actor handles it, not the Spot.
- Waiting for round-end settlement inside a Spot turn also stops the Spot. Keep the Spot in a state that accepts no new round during settlement, or, for a SpotWide User Spot or Instance Spot, release the gate with `Yield` and re-check state after resuming.
- If player data is sharded so that one transaction is impossible, use idempotent writes keyed by the round ID.

### 10.2 Do Not Rebuild Execution Order

| Rule | Common mistake |
|---|---|
| Spots and Actors process one message at a time. Do not guard their state again with locks, semaphores or mutexes. Do not keep state in process-wide static stores serialized by per-key locks | Matchmaking and participation state lived in a process-wide static dictionary, serialized with a semaphore per key. With two nodes, memory diverges. A mutex on player state guarded the Actor's execution order a second time |
| The writer of a value updates memory itself after the commit. Do not update in-memory stores from database commit hooks (interceptors) | A hook scanned changed entities on every transaction commit to update the in-memory store. Changes that bypass the commit were missed, and every transaction paid the cost |
| Values changed elsewhere are announced to the owner Actor or Spot with a message | (the fixed shape of the two mistakes above) |

### 10.3 In-Memory State and Failures

- When you create in-memory state, decide at the same time what remains in the database if the process dies now, and how a restart cleans it up. First choose a structure that leaves nothing to refund: reserve bets in the player's in-memory state (spendable = balance − reserved) and debit and credit them together in the round-result transaction. If the process dies, no money has moved, so there is nothing to refund. Only games that must take money during a round, such as buy-ins, need startup cleanup that cancels in-progress rounds and refunds their stakes.
- ZLink guarantees one owner per Actor, but not that a previous node's late database write cannot land, nor business-level exactly-once. Protect game data with unique request IDs, conditional updates and generation tokens. ObjectGeneration is not a write-permission token.
- Location Store records persist over time and across restarts. Give finished players' Actors and personal Spots a cleanup owner from the start. Without cleanup, records pile up and read cost grows even with zero players online. Tests compare only "the records this test created".
- Send money or reward results to players only after the database commit.
- When Actor shutdown and a request cross, retry only on the "being removed" reply and the error kind that means it. Calling destroy on a timeout or handler error removes an Actor that is alive and may be connected. Return every other failure to the caller.
- When an outside write makes a read model stale, mark only the changed part stale and re-read only for requests that read it. Marking the whole read model stale after every game result makes the next request re-run dozens of statements.

### 10.4 Actor Moves and State

An Actor moves to another node when it joins a Spot and when it is relocated. Everything that must survive the move has to be carried by the relocation adapter. For the per-language names of the policy and the adapter, follow each language's Actor guide.

- Register a stateful Actor with the state-preserving relocation policy (.NET `PreserveStateWith`). The recreating policy (.NET `RecreateOnRelocation`) builds a new Actor and discards application state.
- Gather an Actor's mutable fields into one state record. The adapter saves and restores the whole record instead of picking fields one by one. Keep recomputable caches outside the record, with a comment that they are rebuilt after restore.
- Test: fill every field of an Actor with non-default values, save and restore it, and compare the whole record. Make the test fail for a newly added field until that field is compared too.

### 10.5 Data a Spot Needs When Players Enter

Decide in this order:

1. A player's data (deck, gear, balance) is player Actor state. The Actor enters the Spot carrying its state, so the Spot reads the entered Actor's state directly, without sending a message. If each Actor truly needs separate handling on entry, register it with `Defer()`.
2. Room-wide data (game settings, table rules) is Spot state. Read it once when the Spot is created.
3. Large data used only by the Spot, which Actors do not normally hold, is rare. Only when it is truly needed:
   - Do not request it per player. Before the game starts, the Spot fetches it for all participants in one request (the same shape as round settlement).
   - Meanwhile the Spot stays in a "preparing" state and keeps handling enter and leave messages. The game starts after the data arrives. A SpotWide User Spot or Instance Spot waits with `Yield` and re-checks the participant list after resuming.
   - Register the Join itself with `Defer()`. `Defer()` postpones the Join until the handler ends, so the Spot does not stop waiting for the Join. It does not load data.

### 10.6 Code Structure — Light Hexagonal

Add layers only to separate rules from external technology. If the game rules are simple or there is only one external connection, do not add layers.

| Role | What it does in a game node | What it does not do |
|---|---|---|
| domain | Pure rules and state transitions such as round progress and settlement math. Synchronous functions | ZLink, database or network calls, async |
| Spot and Actor handlers | Own the state. Receive messages, call the domain, and call ports only on connect and at round end | Generic database reads, creating another state owner |
| port | One capability the game needs: `loadPlayer`, `commitRound`, `settleBuyIn` | A generic `Repository<T>` with read and save CRUD |
| adapter | Implements a port with a database or external API, translating external field names into game terms | Game rule decisions |

- Separate the domain model. Keep game state types such as rounds, hands and wallets apart from message DTOs and database row types, and convert once at the handler and adapter boundary. Game rule code must not change when a message or database schema changes.
- Do not add pass-through layers. If `Handler → Service → DomainService → Entity` only forwards the same call, merge the layers.
- Do not add an application service outside Spots and Actors; it would create a second state owner.
- The HTTP lobby is stateless, so the usual hexagonal layout is fine there. Its ports are still defined per capability.

### 10.7 Sessions and Login

- Handle one player's logins in one order (that player's Actor or a per-player queue). Do not put an await between "check" and "register" on a shared map outside an Actor. Two simultaneous first logins both bind and break duplicate-login protection.
- A player's Actor exists in exactly one place in the whole mesh. A duplicate login (the same Actor in two places) is a defect in the Framework or in login handling; fix its cause. Re-syncing the balance from the database before a game starts is only a stopgap until that fix.
- If the login reply has a game to return to (resume), go to that game, not the lobby.
- Clean up only with the ActorRef found during cleanup, inside that player's order. An ActorRef's generation tells creation incarnations apart and carries no binding.

### 10.8 Handlers and Execution Order

- Do not block in handlers or HTTP threads. Do not do other work while holding a transaction open (blocking bridge calls, idle in transaction).
- Do not await your own Spot's or Actor's lifecycle operations (close, destroy) inside a handler. They wait on the same execution order and stall.
- Do not send a request to another Actor or Spot and await its reply inside a turn. If the other side sends a request back, both wait until timeout, and other players' requests to the same Spot are blocked meanwhile. Send state notifications with send (no reply awaited) and let the state owner decide.
- Propagating a versioned value such as a balance needs only send. If the other Actor does not exist, do not create it; it reads the database when it is next created.
- The Spot class a handler targets must be exactly the actual Spot class. A handler targeting a base class does not receive messages for a derived Spot.
- Tell acceptance replies from completion notices apart. An OK reply does not mean the work that starts afterwards has finished.
- Do not bump the revision of public state with a private acknowledgement message (such as READY). Other participants' READY messages are then rejected with a stale revision.

### 10.9 How to Use ZLink

- The Framework waits for remote request connections to become ready. Do not add sleeps, retries or longer timeouts in game code.
- Do not paper over Framework defects in application code. Build a minimal reproduction that uses only the public API and file a ZLink issue.
- One composition root decides startup order: ZLink first, application second. Otherwise the application stops after the runtime on shutdown.
- Before comparing versions, check the Framework and Core versions actually loaded.
- Location Store keys use NUL as the separator, which looks like a space in logs. Do not build keys as strings yourself and compare them.

### 10.10 Design and Testing Habits

- One decision, one place that decides it. Before adding a decision, find the place that already makes it. When several places decide the same thing (such as choosing the dealer) differently, results diverge from round to round.
- Observation has one source. Do not mix query replies and notifications to judge state.
- Take test expectations from the spec, not from the implementation. Check random outcomes exactly in unit tests with fixed inputs.

### 10.11 Checklist — Before Merging Game Node Code

- [ ] No database reads while handling a request
- [ ] One database write transaction per round (per transaction)
- [ ] No Spot handler awaits a remote request or database reply during a game
- [ ] Round results are written per round in one transaction, not per Actor, and Actors are told after the commit
- [ ] Values many players see are held by node memory or one owning Spot
- [ ] Exactly one process writes each value
- [ ] No locks, semaphores or static stores guard Spot or Actor state (Spots and Actors serialize)
- [ ] No cycle of Actors or Spots awaiting requests to each other
- [ ] What remains if the process dies now, and the restart cleanup, are decided
- [ ] Retries happen only on "being removed"; other failures go back to the caller
- [ ] Finished players' Actors and personal Spots have a cleanup owner
- [ ] Stateful Actors use the state-preserving relocation policy and have a save/restore test with every field filled
- [ ] No per-player remote requests on Spot entry
- [ ] No blocking waits, sleeps or retries in handlers
- [ ] Money and reward notices are sent after the commit
- [ ] Game rules are tested without ZLink or a database; the database is reached through capability ports, with no generic CRUD repository

## 11. Related Documents

- Full usage of request/send/pub-sub, writing a handler, and the `async` execution model:
  [Channel Messaging](20-channel-messaging.en.md)
- Spot kinds, execution model, handler lifetime, and DI scope: [Activation and Lifetime](34-activation-lifetime.en.md)
- Host lifecycle and operations: [12-operations](12-operations.en.md)
- Registration points and layering: the [01. Overview](01-overview.en.md)
- The full interface, attribute and context set: [Key type index](13-interface-catalog.en.md)
- Runnable sample code: [14-samples](14-samples.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
