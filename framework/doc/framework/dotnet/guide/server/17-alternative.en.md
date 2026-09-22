---
title: "17. Where ZLink Applies — Internal Service Communication and Real-Time State Servers · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/17-alternative.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# 17. Where ZLink Applies — Internal Service Communication and Real-Time State Servers

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: How Session Binding Works](39-session-binding.en.md) | [Next: 12. Operations — Runtime Metrics · Graceful Drain · Readiness](12-operations.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/17-alternative.en.md) · **C#/.NET** · [Java](../../../java/guide/server/17-alternative.en.md) · [Kotlin](../../../kotlin/guide/server/17-alternative.en.md) · [Node/TypeScript](../../../node/guide/server/17-alternative.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can decide whether ZLink belongs in your system, and tell apart the places it does not.

ZLink is a server-to-server and real-time messaging layer. One framework provides logical
channels, connection lifecycle, Spots, pub/sub and location-based automatic connection together.
If you are building internal service communication or a real-time state server and weighing gRPC
or Akka/Orleans, ZLink is a candidate to take their place.

It applies when "where the service is", "where the client is connected" and "how to serialize a
state unit such as a room, a zone or a symbol" keep coming up as recurring problems. Where
[Overview](01-overview.en.md) covered why they are needed, this chapter checks that judgement at
the level of choosing a technology.

## 1. Where It's Used, at a Glance

Draw the boundary first. **If a monolith or modular monolith is enough, don't reach for
ZLink first.** A call between modules in the same process is just a function call — it
doesn't need server-to-server transport. ZLink is a tool for when you already have a reason
to split across several processes/servers, and you want to cut the complexity of the
communication, connection, routing, and state dispatch between them.

| Situation | Why ZLink helps | Feature used |
|------|--------------------|-----------|
| Internal services calling each other often | Call by **channel name** instead of host/port/stub | channel + location store |
| Broadcasting an event to several services in real time | **Transport fan-out** without a separate broker | fanout pub/sub |
| A dynamic state unit like a game room, chat room, or ride zone | Lock-free serial state processing via a **single execution queue** | Spot |
| A long-lived connection to a mobile/game client | The framework owns connection lifecycle, framing, and the reconnect flow | STREAM |
| Separating the connection server from the logic server | **Reconnect portability** through ActorId-based binding | session actor dispatch |
| **Services implemented in different languages calling each other** | **Interoperable calls** over the same channel contract, on top of a language-neutral wire protocol + codec | cross-language binding |
| Ultra-low-latency HFT, a durable queue, a public external API | **Not ZLink's core territory** | keep gRPC/REST/Kafka/FIX |

## 2. Where It Applies — Places That Need Live State and Order

### 2.1 Building a Real-Time Game Server

**What makes it hard.** Game servers have no standardized framework like the web's
`ASP.NET Core`/Spring. This isn't an accident — there's a reason.

- **The network topology each genre needs is different.** Any web service is shaped the same
  way — "client request → server response" — which is why a framework could standardize
  around it. Games aren't. A board game needs room-based matching and turn progression, an
  MORPG needs a split between room/stage servers and matching/lobby, an MMORPG needs a
  zone/field server mesh and large-scale broadcast, an FPS needs a low-latency tick loop for
  a small session. **Genre decides the topology, so there's no one fixed shape**, and every
  team re-builds its own topology on top of raw sockets.
- **State stays in memory.** The web can put state in a DB and scale out statelessly, but a
  game keeps room/participant state **in-memory** for fast processing and runs its logic
  across multiple threads. That's the moment locks, contention, deadlocks, and the
  synchronization question "which thread is touching this room" seep into business logic.
- **The connection itself is something to manage.** Users keep long-lived connections. You
  handle socket framing and session lifetime directly, have to reconnect a user to whichever
  server and room they were in, and have to keep connected users and in-progress game state
  alive during a deployment or scale-down.

So up to now there were two choices — build all of this yourself, or **move to a separate
runtime**, a game server engine, and relearn how you write logic, configure, deploy, and
operate, on the engine's terms.

**How it's actually been built.** Using the names common in the industry, these approaches
fall into patterns the industry has names for. Boxes like login/auth, gateway, and DB cache show up
repeatedly no matter which pattern you pick — but since there's no common framework backing
them, a team picks its genre's pattern and rebuilds that structure from the socket up.

<iframe class="zlink-diagram" src="/common/diagrams/01-arch-existing-en.html" title="Game backend patterns — existing approach" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-arch-existing-en.html" target="_blank">↗ View larger</a></p>

- **① Zone-sharding.** The world is split into geographic regions, one server (node) owns
  each region, and when a character crosses a boundary the simulation hands off to the
  adjacent region's server. This is the representative scaling approach for an MMORPG
  handling a large open world. Sharding (replicating the whole world and splitting players
  across copies) and instancing (spinning up several independent copies of the same region)
  are also common world-distribution approaches used together to handle a large number of
  concurrent players.
- **② Lobby + room.** Users are received in the lobby/matching stage and assigned to a room,
  which owns participant state until that match ends. A room is usually a logical unit, with
  several running together inside one process. Common in casual, mobile MO, and board games.
- **③ Session-based dedicated fleet.** Once matching tickets accumulate, the fleet assigns
  one dedicated server process for that match, and the client connects directly to that
  server. The process is returned once the match ends. Unlike ②, **one match = one process**
  is the base unit. The standard configuration for session-based games like competitive FPS
  and battle royale.
- **④ Stateful actor.** Entity state, like a player or guild, is kept as an actor in server
  memory, and the DB only serves as periodic storage. It reduces read-heavy load and removes
  the need for a separate caching layer, so it's commonly used for meta/social backends. The
  representative frameworks are Orleans and Akka. **One conceptual difference** — Akka's
  actor isn't one user, it's a general-purpose concurrency unit used anywhere, and ZLink
  splits this into Spot (an execution-isolation unit) and Actor (a domain entity). What's
  closer to Orleans's virtual actor/grain isn't ZLink's Actor — it's the **Instance Spot**
  this approach uses. The detailed comparison is covered in
  [Chapter 17 §6](17-alternative.en.md).

**What ZLink provides.** A feature answers each difficulty, one by one.

| Difficulty | ZLink feature | Details |
| --- | --- | --- |
| Building a genre's topology from raw sockets | **Declare topology by combining channels** — 1:N request/response, fan-out, a node-addressed route mesh, a room-scoped spot mesh, all composed in a few lines of registration; the location store keeps connections up automatically | [Layering and registration points](01-overview.en.md#33-layering-and-registration-points) · [Channel Messaging](20-channel-messaging.en.md) · [Spot](21-spot.en.md) · [Location](25-location.en.md) |
| Locks/contention on in-memory state | **SPOT serial execution** — every message for one room enters its Spot queue and runs in order. Locks disappear from business logic | The code below · [Spot](21-spot.en.md) |
| Implementing socket framing/session lifetime directly | **STREAM** — the framework owns connection lifetime, framing, and packet codec (TCP/TLS/WS/WSS) | [09](23-stream.en.md) |
| Tracking a reconnected user's location | **Actor binding** — a new connection after reconnect picks up the same actor | [08](24-actor-session.en.md) |
| Users dropped during deployment | **Graceful drain** — blocks new admission, hands off actors, finishes in-progress work, then shuts down. 0 lines of app code | [12](12-operations.en.md) |

The patterns above all become combinations on **the same declarative model.**
There's no need to rebuild from the socket for each one.

- **① Zone-sharding** — set up a zone with `AddRouteMesh` + a node-addressed route mesh. A
  player crossing a boundary is handed off by **cross-node actor relocation**
  ([Relocation](37-relocation.en.md)) instead. [ZoneWorld](../../../common/sample/zoneworld/README.en.md)
  is exactly this approach.
- **② Lobby + room** — entry/matching is the Entry Spot, and a room is a room spot created
  with `GetOrCreate`. [Bingo](../../../common/sample/bingo/README.en.md) is exactly this
  approach.
- **③ Matchmaker + dedicated** — matching is implemented as a channel handler (HTTP, etc.).
  **Instead of spinning up a new process per match**, the client connects over STREAM to the
  room spot that was `GetOrCreate`d as the matching result.
  [TicTacToe](../../../common/sample/tictactoe/README.en.md) is closest to this flow —
  matching request → room/connection info response → connect to the already-prepared room
  spot.
- **④ Actor service** — an **Instance Spot** is cold-activated by entity ID and serially
  processes the state of an entity that several users access at the same time, with no Redis
  distributed lock. Continued in the
  [guild service example](#22-concurrent-access-to-one-entity).

Where the "existing approaches" diagram above split into four, here's how each approach
assembles with ZLink, in the same spots.

<iframe class="zlink-diagram" src="/common/diagrams/01-arch-zlink-en.html" title="Game backend patterns — ZLink approach" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-arch-zlink-en.html" target="_blank">↗ View larger</a></p>

Green (bold border) is the SPOT-family primitive. This is exactly where it contrasts with
the "existing approaches" diagram above — each approach used to need its own infrastructure
(a dedicated fleet orchestrator, sticky routing, an actor cluster), but in ZLink, all four
are implemented with the same RouteMesh/Spot/Instance Spot combination. Switching approaches
means no new runtime to learn.

> A Twitch-scale FPS's **ultra-low-latency snapshot netcode** uses unreliable transport that
> tolerates loss. STREAM provides TCP, TLS, and WebSocket transports. Even for that kind of game, though,
> matching/lobby/meta/social are handled by these approaches today. Exactly
> where the line falls is covered in [Chapter 17](17-alternative.en.md) §4.

**How is this different from a game server engine or service?** Alternatives to building
everything yourself include engines and managed services. Comparing what each provides by
area makes ZLink's place clear.

| Area provided | Representative product | Form provided |
| --- | --- | --- |
| Connection/transport optimization — socket/session management, encryption/compression, TCP/UDP in parallel, splitting network I/O from logic threads | [ProudNet](https://docs.proudnet.com/proudnet.eng) | Dedicated server module + client SDK |
| Room/lobby/matching — creating/finding a room, lobby, match invites | [Photon](https://www.photonengine.com/)·[SmartFoxServer](https://docs2x.smartfoxserver.com/Overview/zones-room-architecture) | A room model on its own runtime |
| Hosting/fleet — dedicated server allocation, autoscaling, a matchmaking rules engine (FlexMatch) | [AWS GameLift](https://aws.amazon.com/gamelift/servers/)·Agones | A cloud-managed service |
| Social/meta features — friends, leaderboards, groups, chat | [Nakama](https://heroiclabs.com/nakama-gamelift/) | A backend server product |

ZLink provides **connections/sessions (STREAM), rooms/state units (SPOT), inter-server
messaging (channel), participant state (actor), and zero-downtime termination (host relocation)** among
these — but not as a dedicated runtime or managed service, as a **library layer on the major
framework you already use.**

- **Hosting/fleet isn't ZLink's job.** Whether K8s or GameLift, a ZLink server just runs on
  top of it — it doesn't compete with a hosting service, it composes with one.
- **Matchmaking rules and social features are app logic, not product features.** You write
  them directly with a channel handler and spot. There's less pre-built for you, but the
  ownership and freedom over the logic stay with the app.

Instead of rebuilding for each language, ZLink puts the hard runtime in a single **native
Core (C API)** and wraps it in per-language layers. Per-language **`bindings`** connect that
C API to each language's socket API, and on top a per-language **ZLink Framework** provides
surfaces like RouteMesh · SPOT · actor · STREAM. The reason for this thin 3-layer split is
**multi-language support** — implement the Core once and swap only the language surface, and
C++, .NET, the JVM, and Node share the same core. `bindings` and the Core are the framework's
internal implementation, not exposed on the public API, and application code doesn't change
even if they're replaced later — this backend boundary is explained separately by
[internals/backend-dependency-policy](../../internals/backend-dependency-policy.en.md).

<iframe class="zlink-diagram" src="/common/diagrams/overview-stack-en.html" title="ZLink internal layers — a thin 3-layer stack for multi-language" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/overview-stack-en.html" target="_blank">↗ View larger</a></p>

**As code.** Declare one room, and write that room's progression logic.

```csharp
--8<-- "framework/languages/dotnet/samples/Bingo/Server/Play/PlayServerHostFactory.cs:doc-bingo-play-register"
```

```csharp
--8<-- "framework/languages/dotnet/samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs:doc-bingo-room-join"
```

Several players send requests at the same time and a timer runs in this room, yet there's no
`lock`, no `Interlocked`, no Redis distributed lock. That's because the framework puts
every message for one room (requests, subscription events, timer ticks, actor packets) on
**them into the Spot queue and runs them in order.** Here, "serial" isn't codec serialization
— it's **serialization of execution order** ([The Execution Model](32-execution-model.en.md)).

Runnable reference samples: [TicTacToe](../../../common/sample/tictactoe/README.en.md) ·
[Bingo](../../../common/sample/bingo/README.en.md) · [GameQuest](../../../common/sample/event/gamequest.en.md)

### 2.2 Concurrent Access to One Entity

**Why it's hard.** There are cases, like a guild, where **several different users need to
modify the same entity at the same time.** Just like two users applying to join at the same
time can exceed the roster cap, or two donations landing at once can lose one of them,
several stateless API servers touching the same row at the same time creates a race
condition.

- **Concurrent modifications collide.** If several API instances read-modify-write the same
  guild row at the same time, a lost update happens.
- **You have to assemble your own serialization mechanism.** A Redis distributed lock or DB
  row lock has to build a per-guild critical section.
- **The lock itself is a new failure mode.** Lock acquisition failure, timeout, deadlock, and
  a stale write after lock expiry all land on the app to handle.

**What ZLink provides.** Instead of assembling a lock, it turns that entity into a serial
execution unit.

| What you used to assemble | ZLink feature | Details |
| --- | --- | --- |
| A Redis distributed lock per guild id | **Instance Spot** — one spot, cold-activated by guild id, processes every request for that guild serially | [Spot](21-spot.en.md) |
| Lock acquire/release/timeout handling | **Serial execution** — the lock concept disappears entirely; everything is always processed in spot queue order | [The Execution Model](32-execution-model.en.md) |
| Inter-server calls/LB to find the guild spot | **channel name + location store** | [05](20-channel-messaging.en.md)·[10](25-location.en.md) |
| Pre-provisioning a new guild | Cold-activated on the spot when the first request arrives — no separate preparation needed | |

**The existing approach** — lock acquire/release makes a round trip on every request.

<iframe class="zlink-diagram" src="/common/diagrams/01-guild-existing-en.html" title="Guild state change — existing approach" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-guild-existing-en.html" target="_blank">↗ View larger</a></p>

**The ZLink approach** — the lock disappears, and the guild id itself becomes the spot
address the request will arrive at.

<iframe class="zlink-diagram" src="/common/diagrams/01-guild-zlink-en.html" title="Guild state change — ZLink approach" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-guild-zlink-en.html" target="_blank">↗ View larger</a></p>

A request for the same guild always passes through the same GuildSpot's queue, so the second
request is only processed once the first finishes — it's not that another request is blocked
for as long as the lock is held; two requests can never touch the same state at the
same time in the first place.

**As code.** Where lock acquire/release used to sit, one call remains.

```csharp
--8<-- "framework/languages/dotnet/samples/ShoppingMall/Server/CommerceApi/Infrastructure/ZLink/ZLinkOrderWorkflowRouter.cs:doc-sm-api-request"
```

There's no runnable reference sample for this scenario yet — the code above applies the same
API surface as GameQuest's `PlayerQuestSpot` registration/call approach to a guild.

### 2.3 Adding Real-Time Features to an Existing Web Service

**Why complexity goes up.** Consider a **food-delivery order app**: placing and viewing an
order are ordinary HTTP requests and responses, but status updates such as "preparing → out
for delivery → arriving soon" need to be pushed in real time without requiring the user to
refresh the app. The standard shape of a large web service — Spring/`ASP.NET Core` + Redis
(cache) + Kafka (events) + LB/K8s — is optimized for **stateless request/response.** The
moment you add a real-time feature like this, those assumptions stop fitting one by one, and
complexity rises.

- **The connection becomes state.** An HTTP request can land on any instance, but a
  WebSocket connection is pinned to one specific instance. That's how you end up with a
  sticky LB that pins connections, and the app starts managing "which instance is this user
  connected to right now" in Redis.
- **Real-time delivery between servers has to take a detour.** Since connections are
  scattered across instances, server-to-server delivery routes through a broker (Redis
  pub/sub, or even Kafka when you don't actually need replay) — one more piece of
  infrastructure to operate.
- **Order-sensitive units appear.** For an order or a conversation, the order events are
  processed in is correctness itself. Since several instances could touch the same order at
  the same time, you serialize with a distributed lock.

Adding one feature brings a whole set of components with it — a WebSocket server, sticky
LB, broker detour, distributed lock — plus the operational burden of running it.

**What ZLink provides.** A feature answers each piece of the kit.

| What you used to assemble | ZLink feature | Details |
| --- | --- | --- |
| A WebSocket server + sticky LB | **STREAM** — the app server receives client connections directly | [09](23-stream.en.md) |
| A distributed lock for ordering | **SPOT owner routing** — the same order/conversation always executes serially in its own one Spot | [Spot](21-spot.en.md) |
| Real-time delivery through a broker | **channel/fanout** — inter-server delivery and fan-out go through transport directly | [05](20-channel-messaging.en.md) |
| Managing "who's connected where" | **Actor binding + location store** — the framework owns reconnect portability and location lookup | [08](24-actor-session.en.md)·[10](25-location.en.md) |

Drawing the same food-delivery order app — HTTP order processing + real-time delivery-status
pushes — both ways shows the difference right in the picture.

**The existing approach** — the components for the real-time feature (orange) add up to as
much as the main body.

<iframe class="zlink-diagram" src="/common/diagrams/01-delivery-existing-en.html" title="Existing approach — food-delivery order app" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-delivery-existing-en.html" target="_blank">↗ View larger</a></p>

**The ZLink approach** — every orange piece disappears, leaving one location store that
provides node/actor/spot location information.

<iframe class="zlink-diagram" src="/common/diagrams/01-delivery-zlink-en.html" title="ZLink approach — food-delivery order app" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-delivery-zlink-en.html" target="_blank">↗ View larger</a></p>

The sticky LB, the pub/sub broker, and the distributed lock —
disappear. An **Instance Spot** preserves ordering, **Session servers** (STREAM) handle
real-time connections instead of shell servers, and **direct runtime connections** handle
inter-server delivery. The **location store is the only new infrastructure.**

**As code.** Where the distributed lock and sticky routing used to sit, the following code
remains.

```csharp
--8<-- "framework/languages/dotnet/samples/ShoppingMall/Server/CommerceApi/Infrastructure/ZLink/ZLinkOrderWorkflowRouter.cs:doc-sm-api-request"
```

Runnable reference samples: [SupportChat](../../../common/sample/supportchat/README.en.md) ·
[DeliveryDispatch](../../../common/sample/deliverydispatch/README.en.md)

### 2.4 Simplifying Event-Driven Business Processing

Where ZLink applies isn't limited to real-time features. Business processes like order
handling, settlement, and inventory — where **the same entity's events must be processed in
order, without duplication** — run into the same complexity problem even with zero real-time
push on screen.

**Why it gets complicated.** The standard answer for this kind of work is a log-based
pipeline like Kafka (an event-sourcing setup is usually built on top of this too). But what
the log actually solves is "gather the same key in one place, in order," and a whole train
of pieces follows to get that one thing.

- **Order is tied to a partition.** To process the same order's events in order, you have to
  gather them by key partition, consumer count is tied to partition count, and consumer
  group rebalance and offset management follow as operational items.
- **Consumers are stateless, so state means a DB round trip every time.** Processing one
  event means reading, modifying, and writing current state in the DB every time. Adding a
  cache to cut repeated reads brings an invalidation problem along with it.
- **At-least-once delivery pushes idempotency onto the app.** Redelivery, rebalance, and
  reprocessing can bring the same event twice, so without a version check or a dedupe
  policy, it gets applied twice.
- You build a separate read model to query the processing result, and once the pipeline
  falls behind, lag monitoring and a resync job stay as leftover work.

Keeping state next to the consumer with a stateful stream processor (Kafka Streams/Flink)
cuts the DB round trips, but partition design, state-store recovery, and rebalance remain
your operational responsibility — the detailed comparison is covered by
[GameQuest common scenario §3](../../../common/sample/event/gamequest.en.md).

Drawing the same business process — an order workflow — both ways shows the difference in
pieces right in the picture.

**The existing approach** — the pipeline pieces for ordered processing (orange) add up to as
much as the main body.

<iframe class="zlink-diagram" src="/common/diagrams/01-order-existing-en.html" title="Order processing — existing approach" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-order-existing-en.html" target="_blank">↗ View larger</a></p>

**The ZLink approach** — this doesn't replace Kafka. **On the order-processing path**, the
pipeline pieces (orange) disappear, and Kafka stays in its natural role (gray) — propagating
confirmed facts to independent systems and preserving events that need replay, as a durable
log.

<iframe class="zlink-diagram" src="/common/diagrams/01-order-zlink-en.html" title="Order processing — ZLink approach" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-order-zlink-en.html" target="_blank">↗ View larger</a></p>

The key thing across the two pictures is that Kafka's color changes. Kafka (orange), which
used to own ordering **inside** the processing path, moves **outside** the processing path
and only handles propagation/preservation (gray). And with that, the pieces assembled just
for ordering — the order-processing consumer group (offset/rebalance/dedupe), the cache, the
read model for queries, the resync job — disappear. Since the same `OrderId` is always
processed serially by the same owner, there's no longer a need to assemble the ordering and
duplicate-prevention a pipeline used to provide.

**The inter-server-call LB disappears too.** Order processing calls other services like
inventory and payment synchronously, and the existing approach has to find and distribute to
the peer on every one of those paths via a K8s Service or service discovery (you can't
hardcode the address in code). In ZLink, you call by **channel name**, like `"inventory"`,
**and the location store tells you the currently available peer**, so there's no separate LB
layer needed for inter-server calls — that's why the orange "LB for inter-server calls" is
gone in the after picture.

**What stays, stays.** Client HTTP ingress is still stateless, so an L7 LB/Ingress
distributes to API servers as usual (gray), and order state is still stored in the DB.
Unlike gRPC, this HTTP ingress path also doesn't **additionally** require an L7 distribution
device (the reason is covered in [Chapter 17 §5.1](17-alternative.en.md)).

**What ZLink provides.** Solving "gather the same key in one place, in order" with **owner
routing** instead of a log means most of the pieces above never need to be assembled.

| What you used to assemble | ZLink feature | Details |
| --- | --- | --- |
| Key partition + consumer group | **SPOT owner routing** — the same `OrderId` always executes serially on the same Spot. Whichever API instance receives it, it's routed to the same owner | [Spot](21-spot.en.md) |
| DB load-modify-store per event | **The owner spot's hot state** — state lives in the owner's memory, and the app decides when to persist based on business rules | [Spot](21-spot.en.md) |
| Version check/distributed lock against redelivery | **Serial execution** — no concurrent writer for the same unit, so there's no lock/version contention on the normal path | [The Execution Model](32-execution-model.en.md) |
| LB/service discovery for inter-server calls | **channel name + location store** — call by the name `"inventory"` and it sends directly to a currently available peer | [05](20-channel-messaging.en.md)·[10](25-location.en.md) |
| Operating offset/lag/resync jobs | With no consumption pipeline, that operational item doesn't exist at all | |

**This doesn't replace your existing stack.** Kafka stays exactly where it is, as a durable
event stream, and Redis stays as cache/persistence support. What ZLink cuts is the
**complexity of connection, routing, and state management** you used to assemble by hand in between.

**The boundary stays where it is.** Where a durable log is genuinely needed — event replay,
long-term retention, broad fan-out to independent systems — Kafka is the right fit and stays
exactly there ([Chapter 17 §4](17-alternative.en.md)). What ZLink cuts is the case where a
log pipeline was assembled **only** for entity-scoped ordered processing. If order and
consistency were the entire goal, owner routing achieves that goal directly, with no
pipeline.

**As code.** Where the partition consumer used to sit, an owner Spot handler comes instead.

```csharp
--8<-- "framework/languages/dotnet/samples/ShoppingMall/Server/OrderWorkflow/Infrastructure/ZLink/Spots/OrderWorkflowSpot/OrderWorkflowSpot.cs:doc-sm-spot-start"
```

Runnable reference sample: [ShoppingMall](../../../common/sample/event/shoppingmall.en.md) —
the reference sample for this exact situation, built with no real-time push at all, just an
HTTP API + order workflow. It verifies order state transitions, compensation flow, duplicate
prevention, and projection rebuild, all on top of owner routing.

The situations above differ only in entry point — the surface you use is the same. Products
exist that provide one feature each — gRPC for RPC, Orleans for actors, a game engine for
connections — but ZLink's niche is the **combination** of major-framework
integration, serial-execution state units, and auto-connect topology.

## 3. The Development Model — What the Framework Owns

What ZLink reduces is not only the number of infrastructure components. The decisions about
location, connection, correlation and serialization **move from the application into the
framework.** The application deals only with domain units (channel/Spot/session).

- **You make calls knowing only the channel name** — you don't know the target host/port/stub.
- **Service location and peer distribution** are handled by location-store-based
  auto-connect ([Location](25-location.en.md)).
- **Request correlation and waiting for a reply** are handled by the framework.
- **Client connection lifecycle and packet framing** are handled by STREAM.
- **Serial state for a room/zone/symbol** is handled by the Spot execution queue.
- **Actor/session binding after a reconnect** is carried forward by the framework.
- **The handler/filter/DI model** matches how existing web frameworks work, so it feels
  familiar.

> ZLink doesn't eliminate these problems — it **moves them from caller code into the framework.**
> The framework handles location, connection, correlation, and dispatch serialization, so
> application code reads like **business flow**, not transport configuration.

### 3.1 Several Languages on One Channel (Cross-Language)

ZLink isn't tied to one language. Because the call contract is a **language-neutral wire
protocol (ZMP) + codec (protobuf/json/messagepack) + a logical channel/packet name**,
services implemented in different languages **call each other over the same channel**. For
example, in a game system you could put **the room server in C++ and the API/matchmaking
server in .NET or Java**, and message over the same channel/Spot contract.

- The cross-language contract is a **packet name + a codec-encoded DTO** (protobuf is
  recommended across languages, or an agreed JSON/MessagePack schema). Unlike gRPC, it
  doesn't force service-stub code generation or HTTP/2 — only the payload schema is shared.
- Each language binding lays a handler/Spot/STREAM surface on top of the same core (C ABI,
  ZMP). So even when the handler is written in a different language, on the wire it's the
  same channel and packet.

> **Different-language bindings.** The same channel/packet contract is implemented by each
> language's binding in its own language. This guide's examples are split into language
> tabs, and whichever tab you look at describes the same contract. Cross-language is a
> **design goal** of ZLink — the call contract doesn't depend on the binding's
> implementation language.

### 3.2 How It Feels Compared with the Existing Approach

The difference in the amount of code needed to wire up the same "inter-server
request/response."

**Directly with raw bindings (conceptual)** — not runnable code, but the list of work a
direct implementation would require. Supported languages use the same list, so it isn't
split into language tabs.

```text
Location-store lookup, connecting the endpoint, reconnect management,
correlation id matching, serialization, receive loop ... dozens of lines of connection/setup code
```

**ZLink Framework** — the blocks below are the tutorial's real "profile" channel code
(handler registration, server registration, client call). The only difference from a price
lookup is that the target is a player profile lookup instead.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Channel/GetPlayerProfileHandler.cs:channel-request-handler"
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-request-call"
```

The connection/setup code disappears, leaving a handler and a few lines of channel
registration.

## 4. Symptoms That Make ZLink a Candidate

Judge by **symptoms**, not by technology names. If the following keep recurring, ZLink is a
candidate.

- The gRPC stub, channel factory, deadline, and service-location lookup setup repeat for
  every service.
- gRPC load isn't spreading evenly under a Kubernetes L4 LB, so you're considering a mesh.
- You're protecting a state unit like a game room, chat room, or ride zone with a lock.
- You separately track, in Redis, which server a client was connected to before a reconnect.
- You're using Kafka for real-time event fan-out, but you don't actually need replay.
- External client connections, internal service calls, and room-state processing are spread
  across different frameworks.

## 5. What ZLink Doesn't Do — the Boundary

For the benefits to be clear, the boundary has to be clear too. The following are best left
as-is.

| Requirement | ZLink guidance |
|------|------------|
| A public-facing external HTTP API | Keep REST/gRPC |
| A durable queue, replay, consumer offsets | Keep Kafka/NATS |
| DB queries, geo-index, audit trail | Keep DB/Redis/event store |
| An HFT microsecond matching loop | Keep Disruptor/Aeron/FIX |
| Internal service communication + real-time state dispatch | **ZLink fits** |

The point: ZLink is a transport/dispatch layer, **not a datastore, a durable log, or an HFT
bus.** Hard domain problems like distributed data consistency (saga, outbox, idempotency)
and persistence/duplicate control remain the application's and infrastructure's
responsibility.

## 6. Reference — Comparison with the gRPC/Service-Mesh Stack

To see why "internal services calling each other often" in §1 makes ZLink a candidate,
compare it with the gRPC stack.

### 6.1 The Limits of gRPC Alone

gRPC's own performance is excellent. The problem is that the official best practices for
making this kind of service **"production grade"** immediately call for additional
infrastructure.

- **Reusing channels/stubs is mandatory.** "Always re-use stubs and channels when possible"
  — creating a channel per call adds latency, so you manage the lifecycle
  yourself with a channel factory/pool.
  ([grpc.io performance](https://grpc.io/docs/guides/performance/))
- **A deadline on every call.** You attach a deadline so one slow RPC doesn't block an
  upstream service.
  ([Microsoft Learn](https://learn.microsoft.com/en-us/aspnet/core/grpc/performance))
- **The default load balancer (L4, per-connection distribution) doesn't spread gRPC load
  evenly.** Because gRPC keeps one connection open for a long time over HTTP/2 and
  multiplexes many requests over it, an L4 load balancer sees only one connection, and
  requests pile onto whichever server that connection first connected to. Since it's built on
  HTTP/2, per-request (L7) distribution is effectively required, so you typically add one of
  the following on top.
  - **Client-side LB**: the client holds the server list and calls them in rotation itself.
  - **A headless service** (Kubernetes): exposes the service not as one virtual IP but as
    **the IP list of each backing pod**, so the client distributes evenly on its own.
  - **An Envoy/Istio service-mesh sidecar**: a **proxy** auto-deployed alongside each
    service handles per-request (L7) distribution and encryption (mTLS) on its behalf.
  ([Kubernetes blog](https://kubernetes.io/blog/2018/11/07/grpc-load-balancing-on-kubernetes-without-tears/))
- **On top of that**, service-location lookup (Eureka/Consul/xDS), retry/hedging, the
  `.proto` pipeline, mTLS, and **yet another separate broker for event fan-out**
  (Kafka/NATS).

L7 distribution splits work by looking at each individual request, not the connection — a
mesh sidecar or client-side LB plays this role.

<iframe class="zlink-diagram" src="/common/diagrams/17-l7-distribute-en.html" title="L7 distribution — split request by request" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-l7-distribute-en.html" target="_blank">↗ View larger</a></p>

In other words, "using gRPC" really means running **gRPC + an L7 LB (usually a mesh) +
service-location lookup + an event broker + a proto pipeline** together.

### 6.2 Deployment Shape Comparison

<iframe class="zlink-diagram" src="/common/diagrams/17-classic-mesh-en.html" title="Classic — gRPC + service mesh + broker + WS edge" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-classic-mesh-en.html" target="_blank">↗ View larger</a></p>

<iframe class="zlink-diagram" src="/common/diagrams/17-zlink-channel-en.html" title="ZLink — framework + location store" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-zlink-channel-en.html" target="_blank">↗ View larger</a></p>

The Envoy sidecar and mesh control plane (service-location lookup, L7 LB, mTLS) give way to
one layer: the framework plus the location store. The broker and the WS edge
can be absorbed into the fanout channel and STREAM if the requirements are limited to real-time
propagation and connection admission; if you need a durable queue with replay, or HTTP-edge
policy, you keep them as-is.

### 6.3 The Path One Call Takes

<iframe class="zlink-diagram" src="/common/diagrams/17-sidecar-path-en.html" title="Sidecar path — Envoy local to Envoy remote, two hops" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-sidecar-path-en.html" target="_blank">↗ View larger</a></p>

<iframe class="zlink-diagram" src="/common/diagrams/17-channel-path-en.html" title="channel path — direct call, no sidecar" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-channel-path-en.html" target="_blank">↗ View larger</a></p>

### 6.4 Components That Go Away with ZLink

| gRPC best practice/required infrastructure | In ZLink | Note |
| --- | --- | --- |
| "Reuse stubs/channels" | The route client is a DI singleton and the framework manages the MeshNode connection lifecycle | Nothing to create per call |
| RPC deadline | `RequestToChannel(...).Timeout(...)` | The reply-wait duration |
| L7 load balancing (Envoy/Istio) | Channel name + store auto-connect distributes traffic across peers | No sidecar needed |
| Interceptor | Handler filter | [5](20-channel-messaging.en.md) §5 |
| Event broker (Kafka/NATS) | fanout channel pub/sub | Real-time fan-out only. A broker stays for persistence/replay |
| Unified observability (mesh telemetry) | The status stream and standard diagnostics | `11. Monitoring` chapter |
| Bidirectional streaming | STREAM session | Admits external clients. HTTP-edge policy is separate |

This comparison isn't trying to generalize which is better. gRPC is still a good choice when
a public external API, a standard RPC contract, or organization-standard tooling matters.
Performance also varies with payload size, codec, network, peer count, and deployment
shape, so no numeric claim is made here. The gain described here is that **the call path and
the number of operational components shrink** — a setup that used to go through an HTTP/2
proxy, a stub, and a separate broker collapses into one layer: the framework plus the
location store. If your organization's security policy or external ingress still needs it,
keep the existing mesh/LB alongside it.

## 7. Reference — Comparison with Distributed Actor Frameworks (Orleans/Akka)

Microsoft Orleans and Akka are representative frameworks used for the ④ stateful-actor
pattern in `01. Overview` §2. Because ZLink's Spot/actor offers the same
primitives (mailbox serialization + location transparency), the candidates overlap for this
workload.

### 7.1 The Limits of Orleans/Akka Alone

Orleans and Akka specialize in **a single actor primitive.** But to build "one real-time
state server," the subject of this guide, you still have to assemble the pieces outside the
actor yourself.

- **No external client connection.** Neither one bundles a protocol for a client to call a
  grain/actor directly. A web client usually connects through SignalR or a separate
  WebSocket server, which then calls into the actor.
- **Not polyglot.** Orleans is `.NET`-only, Akka is JVM-only (Akka.NET is a separate port).
  Combining a C++ room server with a `.NET` API server under the same contract is outside
  their design scope.
- **Service-to-service messaging is separate from actor calls.** Grain-to-grain calls exist,
  but there's no general service-messaging surface such as channel-name-based
  request/response or fanout — if you need one, you add gRPC or a message broker
  separately.

### 7.2 Deployment Shape Comparison

<iframe class="zlink-diagram" src="/common/diagrams/17-orleans-cluster-en.html" title="Orleans/Akka — actor cluster with separate edge" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-orleans-cluster-en.html" target="_blank">↗ View larger</a></p>

<iframe class="zlink-diagram" src="/common/diagrams/17-zlink-integrated-en.html" title="ZLink — integrated stack" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-zlink-integrated-en.html" target="_blank">↗ View larger</a></p>

Client connections, service messaging, and actor state are provided together — collapse
into one. But this diagram doesn't imply that the auxiliary
tooling Orleans/Akka built up over a long time, like persistence connectors and reminder
schedulers, also collapse into one. The table below separates raw feature differences from
differences in the availability of this kind of pre-built tooling.

### 7.3 Feature Comparison — Advantages and Disadvantages

| Item | Orleans / Akka | ZLink |
| --- | --- | --- |
| Actor primitive (mailbox serialization + location transparency) | ✅ | ✅ (Spot/actor) |
| Built-in external client connection | ❌ Assemble SignalR/WS separately | ✅ STREAM |
| Polyglot | ❌ Single language (.NET or JVM) | ✅ |
| Typed inter-service messaging + declared topology | ❌ Assemble separately (gRPC, etc.) | ✅ channel + location store |
| Actor state persistence | ✅ Mature provider ecosystem | ⚠️ Lifecycle hooks exist; no pre-built storage connector (① below) |
| Restoring a Spot timer after relocation | ✅ | ✅ Registration and the pending tick are included in the payload and restored automatically |
| Create a missing Actor or use an existing one | ✅ | ✅ `GetOrCreate` coordinates concurrent creation of the same ActorId |
| Waking a dormant actor at a scheduled time (reminder) | ✅ One API call (Orleans Reminder) | ❌ No dedicated API — compose with a distributed scheduler (② below) |
| Distributed transactions | Orleans has experimental support | ❌ None (the app composes a saga) — this is an inherent protocol challenge that can't be worked around with existing primitives |
| License | Orleans MIT / Akka BSL (a paid trigger based on annual revenue) | framework is FSL-1.1-ALv2, core/binding are MPL-2.0 — no revenue-based paid trigger (§7) |
| Time proven in production | 10+ years (Halo, Microsoft 365, Skype) | Short — this project itself is still in progress |

① **Actor state persistence** — lifecycle hooks like `OnCreateAsync`/`OnClosingAsync` are provided,
but which DB to use and how to persist the state is left to the application to decide. This
means there's no bundle of pre-built storage connectors
([ShoppingMall](../../../common/sample/event/shoppingmall.en.md) is an example of this).

② **Reminder** — the application configures a distributed scheduler, such as Quartz.NET
Clustered or Hangfire, to run an Actor `GetOrCreate` or message at a scheduled time.

**Conclusion.** For this guide's workload — "build one real-time state server without
stitching components together" — ZLink is a viable alternative. The Framework provides
Actor/Spot lifecycle and
relocation-timer restoration. Persistent-state providers and scheduled-time reminders must
be composed by the application with its own storage and scheduler. Distributed transactions
aren't provided either. Whether to migrate an existing Orleans/Akka system should be decided
by weighing these differences together with your operational experience.

## 8. License — the Cost of Using It

A technology choice comes bundled with its license terms. Akka is BSL, which requires a
commercial contract once annual revenue crosses a threshold; Orleans is MIT. ZLink's license
differs by layer.

| Layer | License |
| --- | --- |
| `core`, `bindings` — the messaging engine and per-language native bindings | [Mozilla Public License 2.0](../../../../../../LICENSE) |
| `framework` — the Spot/actor, channel messaging, STREAM, and drain this guide covers | [Functional Source License 1.1, ALv2 Future License](../../../../../LICENSE) |
| Each language's `http-client` package | Apache License 2.0 |

**FSL-1.1-ALv2:** it limits only the sale of products that compete with ZLink, while allowing
other uses. Each release becomes Apache-2.0 two years after publication.

| | |
| --- | --- |
| Allowed | Building and shipping/selling your own product or service, internal company systems, education/research |
| Not allowed | A commercial product or service that replaces ZLink itself or provides substantially the same functionality |
| Cost | None. No usage fee, and no paid-conversion threshold like annual revenue |
| After two years | That release automatically converts to Apache-2.0 |

**Conclusion.** Whether it's a game server or a business server, there's no cost or
restriction on building it, running it as a service, and selling it. There's no trigger like
Akka's BSL that flips to paid once revenue grows large enough.

The reason `core` and `bindings` are MPL-2.0 is that `core` started from
[libzmq](https://github.com/zeromq/libzmq) v4.3.5, which is MPL-2.0. `http-client` is a thin
wrapper around each platform's conventional HTTP client library, so it's Apache-2.0.

The exact terms are governed by [framework/LICENSE](../../../../../LICENSE); the policy
background is documented in
[doc/license/README.md](https://github.com/zlink-systems/zlink/blob/main/doc/license/README.md).

## 9. Related Documents

- Common business scenarios: [Framework Common Sample Scenarios](../../../common/sample/README.en.md)
- How to use it: [Channel Messaging](20-channel-messaging.en.md)
- Surface mapping: [Channel Messaging](20-channel-messaging.en.md), [Key Type Index](13-interface-catalog.en.md)
- Samples as runnable code: [14-samples](14-samples.en.md)

### 9.1 References

- [gRPC Performance Best Practices](https://grpc.io/docs/guides/performance/)
- [Performance best practices with gRPC (.NET)](https://learn.microsoft.com/en-us/aspnet/core/grpc/performance)
- [gRPC Load Balancing on Kubernetes without Tears](https://kubernetes.io/blog/2018/11/07/grpc-load-balancing-on-kubernetes-without-tears/)
- [System Design Study: Netflix's adoption of Service Mesh](https://vivekbansal.substack.com/p/system-design-study-netflixs-adoption)
- [Scaling Microservices: Lessons from Netflix, Uber, Amazon, and Spotify](https://www.netguru.com/blog/scaling-microservices)
- [Orleans overview (Microsoft Learn)](https://learn.microsoft.com/en-us/dotnet/orleans/overview)
- [The impact of the Akka License Change (Coralogix)](https://coralogix.com/blog/akka-license-change/)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
