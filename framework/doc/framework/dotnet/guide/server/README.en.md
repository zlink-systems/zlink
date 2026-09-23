---
title: "Guide Home · C#/.NET"
---

# ZLink Framework .NET — User Guide

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/README.en.md) · **C#/.NET** · [Java](../../../java/guide/server/README.en.md) · [Kotlin](../../../kotlin/guide/server/README.en.md) · [Node/TypeScript](../../../node/guide/server/README.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

A `.NET` application framework for building **server systems where real-time
messaging matters** out of several cooperating processes. It goes straight into
`ASP.NET Core`, so there is no separate runtime to move to.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"
```

Register one handler and the framework takes care of message decoding, routing
and encoding.

---

## Systems It Is Built For

It is designed for systems where several server processes divide the roles
between them and state changes reach the client in real time.

| Domain | Core scenario |
|--------|--------------|
| **Real-time games** | Create a room -> player joins -> game state updates -> client push |
| **Customer support chat** | Open a conversation -> assign an agent -> relay messages -> push conversation state |
| **Order workflow** | Accept an order -> process each step -> change state -> notify the client |
| **Delivery and dispatch** | Request a dispatch -> assign and accept -> track state -> real-time push |

The shape they share is that server processes for each role talk in typed
messages, and the client receives state changes over a real-time connection.

<iframe class="zlink-diagram" src="/common/diagrams/guide-topology-en.html"
        title="Servers for each role talk in typed messages and the client receives over STREAM" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-topology-en.html" target="_blank">↗ Open larger</a></p>

---

## Core Capabilities

### Channel messaging — typed request-reply between servers

A channel is a name given to a path between servers. One side sends a request to
the channel name and the other side handles it and replies. Serialization (JSON ·
MessagePack · Protobuf) is the framework's work.

The sending side.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-request-call"
```

The receiving side.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Channel/GetPlayerProfileHandler.cs:channel-request-handler"
```

Besides request-reply there are the fanout (pub/sub) and route mesh (address
routing) patterns.
[Channel messaging →](20-channel-messaging.en.md)

---

### Spot — a unit of state without locks

A Spot binds **one region of state** and its participants into an execution unit:
a game room, a support conversation, a unit of order processing. Everything that
happens inside one Spot — participant packets, timers, joins and leaves — is
processed **serially**. State is reached without a lock, and two requests never
overlap in the same Spot even when the handling is asynchronous.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Spots/GameRoom.cs:spot-class"
```

It divides into an entry spot that assigns (one per node) and a room spot that
holds the state (one per unit). Periodic work is registered as a timer.
[Spot →](21-spot.en.md)

---

### STREAM and Actor — the client's real-time connection

A client's real-time two-way connection is a **STREAM**, and the server-side
object standing for one connection is an **Actor**. When a client connects, a
session creates the Actor, and the Actor joins a Spot to take part in handling
its state.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Sessions/GameSession.cs:session-actor-relay"
```

The client side of the connection is a separate product, the stream connector.
[STREAM →](23-stream.en.md) · [Binding a session to an Actor →](24-actor-session.en.md)

---

### Location — endpoints stay out of the code

When several servers of the same role are up, the address of the one to connect
to is not written in the code. A shared location store keeps the addresses, and
each server looks up the node an id is on right now.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:location-store"
```

[Location →](25-location.en.md)

---

### What `ASP.NET Core` provides

The DI container, configuration, HTTP endpoints and logging are `ASP.NET Core`'s own.
ZLink Framework registers its handlers and meshes on top of them, so REST
endpoints and real-time connections run in the same process.

---

## Table Of Contents

| Order | Document | Content |
|----|------|------|
| 1 | [Overview](01-overview.en.md) | A quick map of the core surface, layers, and topology |
| 2 | [Quickstart](../../quickstart.en.md) | Install, a minimal project where two processes call each other, first-run checks |
| 3 | [Core Concepts](03-concepts.en.md) | What a channel, a Spot, an Actor and a session each are |
| 4 | [Channel Messaging](20-channel-messaging.en.md) | The path that calls by name — registering and calling |
| 5 | [Spot](21-spot.en.md) | Creating and calling a shared place by id |
| 6 | [Actor](22-actor.en.md) | Creating and calling one entity by id |
| 7 | [STREAM](23-stream.en.md) | A client outside the mesh attaching over one connection |
| 8 | [Session and Actor](24-actor-session.en.md) | Binding one connection to one Actor |
| 9 | [Location](25-location.en.md) | Looking up the node something is on by id |
| 10 | [Monitoring](26-monitoring.en.md) | A placeholder in the feature guide — no body yet |
| 11 | [The Execution Model](32-execution-model.en.md) | Two queues, the serialization scope, the turn |
| 12 | [Backpressure](33-backpressure.en.md) | When arrival outruns processing, and the options that affect it |
| 13 | [Activation and Lifetime](34-activation-lifetime.en.md) | Creation time per kind, lifecycle callbacks, injection lifetime |
| 14 | [Actor Membership](35-actor-membership.en.md) | Moving between Spots, reservations and limits |
| 15 | [Timers and Workers](36-timer-worker.en.md) | Periodic execution, running outside the line, giving the turn back |
| 16 | [Relocation](37-relocation.en.md) | What survives a move, the adapter, the unit |
| 17 | [How Channels Work](30-channel-patterns.en.md) | Pattern differences, target selection, pub/sub, connection and discovery |
| 18 | [Handlers and Message Processing](31-handler-dispatch.en.md) | Registration variants, filters, codecs, handler kinds |
| 19 | [How STREAM Works](38-stream-boundary.en.md) | Startup checks, error ownership, reply tokens, execution mode |
| 20 | [How Session Binding Works](39-session-binding.en.md) | How many bindings, route refresh, disconnect, failures |
| 21 | [Where ZLink Applies](17-alternative.en.md) | Use cases, alternative comparisons, boundaries, and licensing |
| 22 | [Operations and Lifecycle](12-operations.en.md) | Runtime metrics, relocate, drain, readiness wiring |
| 23 | [Options](16-options.en.md) | The option list, the defaults and when to change them |
| 24 | [Picking a Sample](14-samples.en.md) | Choosing which sample to read first and how to run it |
| 25 | [Reading Along: Bingo](50-bingo.en.md) | Authentication, matching, rooms, timers, multicast and cleanup in code order |
| 26 | [Reading Along: TicTacToe](51-tictactoe.en.md) | Manual connection and registration, an Actor join onto another node |
| 27 | [Reading Along: SupportChat](52-supportchat.en.md) | Several Actors on one session, metadata relay, the idle timer |
| 28 | [Reading Along: DeliveryDispatch](53-deliverydispatch.en.md) | One-way sends, deadline records and reassignment, customer pushes |
| 29 | [Reading Along: ShoppingMall](54-shoppingmall.en.md) | The owner Instance Spot, replay, next step and expected version |
| 30 | [Reading Along: GameQuest](55-gamequest.en.md) | A per-player owner, best-effort pushes and reconciliation |
| 31 | [Reading Along: ZoneWorld](56-zoneworld.en.md) | Capacity placement, boundary joins and relocation, fanout and observation |
| 32 | [E2E Testing](15-e2e-testing.en.md) | Verifying the whole system with the client library |
| 33 | [Key Type Index](13-interface-catalog.en.md) | The contract interfaces indexed by their verification code |
| 34 | [Monitoring](26-monitoring.en.md) | Awaiting rewrite — status snapshots and diagnostics |

The file number identifies the same chapter regardless of language. This table owns the
reading order.

Chapters 01, 11, 13, and 16 are written separately for `.NET` because the install steps
and surface names differ per language.

## How To Read The Diagrams

Every diagram in this guide uses the same visual language — the color is the
concept.

<iframe class="zlink-diagram" src="/common/diagrams/guide-element-kinds-en.html"
        title="The five kinds that appear in the diagrams" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-element-kinds-en.html" target="_blank">↗ Open larger</a></p>

Several chapters draw the same topology; what changes from chapter to chapter is
where it is magnified.

## Related Documents

- `.NET` documentation entry point: [ZLink Framework for .NET](../../README.en.md)
- Public contract: [.NET public contract](../../../common/spec/server/languages/dotnet/README.en.md)
- Language-neutral meaning: [Common spec](../../../common/README.en.md)
- Client library: [HTTP client](../http-client/README.en.md) · [Stream connector](../stream-connector/INDEX.en.md)
