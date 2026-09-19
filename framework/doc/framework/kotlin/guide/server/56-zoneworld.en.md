---
title: "Reading Along: ZoneWorld · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/56-zoneworld.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: ZoneWorld

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Reading Along: GameQuest](55-gamequest.en.md) | [Next: 15. E2E Testing — Verifying the Whole System with a Client](15-e2e-testing.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/56-zoneworld.en.md) · [C#/.NET](../../../dotnet/guide/server/56-zoneworld.en.md) · [Java](../../../java/guide/server/56-zoneworld.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/56-zoneworld.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the ZoneWorld sample in an editor and follow, in the code, the path of a player who
    enters the world and crosses a zone boundary onto another node, and the path by which the ops
    console's announcements and maintenance reach every node. The code in this chapter runs as it
    stands in `framework/languages/java/samples/kotlin/ZoneWorld`.

[Picking a Sample](14-samples.en.md#8-zoneworld--building-a-zone-sharded-mmorpg-and-ops-control)
introduced what this sample demonstrates. This chapter is what you read after that introduction — the
roles and where their code lives, the message flow of the main scenarios, and, for each flow, the
framework feature it uses and the chapter that explains it, in the order the source is laid out.
This chapter has no spec document that owns a contract. The requirements, message contract and
verification criteria are owned by the
[ZoneWorld scenario](../../../common/sample/zoneworld/README.en.md), and this chapter does not
restate them.

## 1. What This Sample Demonstrates

The world is divided into zones, and each zone is a User Spot with an id. Which ZoneNode owns which
zone is decided by Location Store placement, not by configuration. A player is an Actor with an id,
and crossing a zone boundary means joining the neighbouring zone Spot — when the owner differs, the
framework moves the Actor, and the client's connection stays as it is. The ops console sends
announcements and maintenance without a list of nodes.

<iframe class="zlink-diagram" src="/common/diagrams/14-zoneworld-en.html" title="ZoneWorld sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-zoneworld-en.html" target="_blank">↗ View larger</a></p>

The flow this chapter follows is entry and binding → moving and the boundary join → relocation and
join completion → the zone tick's pushes and border snapshots → the ops fanout and node observation.
The subject of this sample is that "doing something to several nodes" needs a different surface in
each situation, and the code for each surface appears in that order.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| Gateway | 1 | The game STREAM, player Actor binding, relay and pushes | `Server/Gateway` |
| ZoneNode | 2 | The Entry Spot, zone Spots, player Actors, bot timers, local reports | `Server/ZoneNode` |
| Ops | 1 | The ops STREAM, runtime event collection, fanout publish, the maintenance store | `Server/Ops` |
| Client | browser | The game screen and the ops console | `Client` |

Movement rules and zone coordinates live in `Server/ZoneNode/Domain` and reference no framework type.
Messages are set by the JSON contract in `shared`.

## 3. Server Configuration — Mesh, Capacity and Fanout

ZoneNode registers the Entry Spot, the player Actor factory and the zone Spot factory. The zone Spot
factory declares a per-node capacity — every ZoneNode requests every zone, and the capacity is what
makes each node own only some of them.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-zw-node-register"
```

The player Actor factory names an adapter that preserves state, and the zone Spot factory disables
relocation. Separately from the mesh, the node registers a subscriber on the classic fanout channel.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-zw-fanout-subscribe"
```

The Ops side is the publisher on the same fanout channel.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/Program.kt:doc-zw-fanout-publisher"
```

Placement by capacity is covered by [Activation and Lifetime](34-activation-lifetime.en.md), and the
fanout channel by [Channel Messaging](20-channel-messaging.en.md#5-fanout-channel).

## 4. Entry — Binding and the First Zone Join

When the browser connects to Gateway and sends a join, the session creates or finds the Actor by
`PlayerId`, binds it to the current session, and relays the join packet to that Actor.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-join-move-en.html" title="Entry and movement inside a zone — JoinWorld · Move" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-join-move-en.html" target="_blank">↗ View larger</a></p>

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/gateway/GameSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/gateway/GameSession.kt:doc-zw-session-bind"
```

The Actor starts in the Entry Spot. The Entry Spot's handler derives the zone from the coordinates
and reserves a join into that zone Spot.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-entry-join"
```

The join runs after the handler returns, and its result arrives at the Actor's completion callback.
The entry reply is sent from that callback — receiving it means admission into the target zone has
completed. Binding is covered by [Session and Actor](24-actor-session.en.md), and the reservation
rules by
[Actor Membership](35-actor-membership.en.md#2-reserving-a-join--it-runs-after-the-handler-ends).

## 5. Movement — Inside a Zone and Across a Boundary

A move request is relayed to the bound Actor and arrives at the zone Spot's actor send handler. The
domain decides, and the result splits into a rejection, a move inside the zone, or a zone change.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-move"
```

Inside the zone, the Actor updates its coordinate and sends the copy update to the zone Spot. When
the zone changes, it reserves a join into the target zone Spot.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-zone-change"
```

The application does not distinguish whether the target's owner is the same node or another. On the
same node only the membership changes; on another node the framework moves the Actor inside the
join. Messages that reach the previous owner during the move are forwarded to the target.

## 6. Relocation and Join Completion

When the owner changes, the Actor's application state is serialized by the adapter named on the
factory. It carries only the coordinate, the zone, the bot direction and the pending join; queues and
timers are moved by the framework.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-relocation-en.html" title="Boundary movement and relocation" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-relocation-en.html" target="_blank">↗ View larger</a></p>

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-actor-capture"
```

The target zone Spot's admission is the final judge. If that node is under maintenance it rejects the
join; otherwise it records the join as pending and accepts.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-admission"
```

The join's result arrives at the Actor's completion callback. The callback filters duplicates by the
operation id, then sends the entry reply on acceptance or a failure notify on rejection to the bound
session. A rejected move leaves the coordinate where it was.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-join-completed"
```

What survives a move and the adapter's part are covered by [Relocation](37-relocation.en.md), and
the rule that the connection is kept during the move by
[How Session Binding Works](39-session-binding.en.md).

## 7. The Zone Tick — Pushes and Border Snapshots

The zone Spot ticks on a timer and builds a state notify from its own zone and the neighbouring
snapshots. The notify is sent to each player Actor, and the Actor's handler pushes it to the bound
session. A bot is an Actor of the same type without a bound session.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-state-push"
```

The state near the border is published on a topic per neighbouring zone. Both the sending and the
receiving zone are in the topic name, so unrelated zones never receive it.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-border-publish"
```

Each zone Spot subscribes only to the topics coming from its own neighbours.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-border-subscribe"
```

Timers are covered by [Timers and Workers](36-timer-worker.en.md#1-timers--periodic-execution), and
Logical Multicast that picks targets by topic by
[How Channels Work](30-channel-patterns.en.md#5-the-forms-of-pubsub).

## 8. Operations — Fanout and Node Observation

Ops publishes announcements and the maintenance desired state over fanout. The publisher holds no
list of nodes — adding a node changes nothing on this side.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-ops-en.html" title="Ops observation, announce and maintenance" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-ops-en.html" target="_blank">↗ View larger</a></p>

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/OpsSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/OpsSession.kt:doc-zw-ops-publish"
```

The subscriber on each ZoneNode applies only its own `NodeId`'s share to the local policy. Even when
this cache is stale, the final judge is the target admission of the previous section.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/ZoneDomain.kt:doc-zw-maintenance-subscriber"
```

Whether a node is registered and connected is learned by observing runtime status, not by a request.
Ops observes the mesh's status and turns changes in the set of Ready peers into node connection
state.

`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/NodeLivenessObserver.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ZoneWorld/Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/ops/NodeLivenessObserver.kt:doc-zw-observe-peers"
```

The forms of fanout are covered by [How Channels Work](30-channel-patterns.en.md#5-the-forms-of-pubsub),
and runtime status and drain by [Operations and Lifecycle](12-operations.en.md).

## 9. Running and Verifying

One runner starts the Redis container, the server processes and the browser scenario together.

```bash
framework/languages/java/samples/kotlin/ZoneWorld/run_sample.sh
```

The browser scenario checks the boundary move, the connection that survives relocation, the arrival
of announcements, and the admission rejection under maintenance. The checks and the scenario ids are
set by the [ZoneWorld scenario](../../../common/sample/zoneworld/README.en.md#9-client-self-check).

## 10. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [ZoneWorld scenario](../../../common/sample/zoneworld/README.en.md)
- The same path — an Actor joining a room on another node — in the smallest layout:
  [Reading Along: TicTacToe](51-tictactoe.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
