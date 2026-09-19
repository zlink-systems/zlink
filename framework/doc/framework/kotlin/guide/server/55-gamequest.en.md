---
title: "Reading Along: GameQuest · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/55-gamequest.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Reading Along: GameQuest

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Reading Along: ShoppingMall](54-shoppingmall.en.md) | [Next: Reading Along: ZoneWorld](56-zoneworld.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/55-gamequest.en.md) · [C#/.NET](../../../dotnet/guide/server/55-gamequest.en.md) · [Java](../../../java/guide/server/55-gamequest.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/55-gamequest.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can open the GameQuest sample in an editor and follow the code from a client's gameplay
    action, through the decision in the per-player owner Spot, to the progress and completion
    pushes, and through the reconciliation of lost progress. The code in this chapter runs as it
    stands in `framework/languages/java/samples/kotlin/GameQuest`.

[Picking a Sample](14-samples.en.md#7-gamequest--building-a-quest-progression-system) introduced what
this sample demonstrates. This chapter is what you read after that introduction — the roles and where
their code lives, the message flow of the main scenarios, and, for each flow, the framework feature it
uses and the chapter that explains it, in the order the source is laid out. This chapter has no spec
document that owns a contract. The requirements, message contract and verification criteria are owned
by the [GameQuest scenario](../../../common/sample/event/gamequest.en.md), and this chapter does not
restate them.

## 1. What This Sample Demonstrates

GameApi validates the client's action and turns it into a gameplay event, and one owner Spot on
QuestMission decides every event of the same `PlayerId` in order. The decision is recorded in an event
stream, and progress and completion are pushed to the connection through the player's session Actor.
The layout is the same owner Spot plus event sourcing as ShoppingMall, but the progress-tier messages
are best-effort — when one is lost, the authoritative fact is read and the progress is reconciled.

<iframe class="zlink-diagram" src="/common/diagrams/14-gamequest-en.html" title="GameQuest sample topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-gamequest-en.html" target="_blank">↗ View larger</a></p>

The flow this chapter follows is join and the session Actor binding → action intake and the send to
the owner → the owner Spot's decision and recording → the progress push → reconciliation and closing.

## 2. Roles and Where the Code Lives

| Role | Processes | Owns | Code |
| --- | ---: | --- | --- |
| GameApi | 2 | STREAM sessions, session Actors, action validation and gameplay events, owner calls | `Server/GameApi` |
| QuestMission | 2 | The player quest Instance Spot, replay, decision, append and projection, notifies | `Server/QuestMission` |
| Client | 1 | The join, action, reconnect and reconcile assertions | `Client` |

Quest conditions, the aggregate fold and the completion rule live in `Server/QuestMission/Domain` and
reference no framework type. Action validation and event creation belong to
`Server/GameApi/Application`. Messages are set by the JSON contract in `shared`.

## 3. Server Configuration

GameApi registers the Entry Spot and the session Actor factory as an object server and receives STREAM
connections. QuestMission registers the player quest Instance Spot factory on the same mesh.

`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:doc-gq-api-register"
```

The QuestMission side registers only the Instance Spot factory on the mesh's object server.

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-mission-register"
```

Both roles share one mesh and no per-mission channel name exists. The Instance Spot factory chooses
the policy that recreates the Spot on the target during relocation — the state can be restored from
the event stream. The creation policy of each kind is covered by
[Activation and Lifetime](34-activation-lifetime.en.md#1-the-kinds-of-spot).

## 4. Join and the Session Actor

When the client joins, GameApi's session handler creates or finds the session Actor by `PlayerId` and
binds it to the current session. This Actor holds no quest state — it is the place that carries the
notifies sent by the owner Spot onto the connection.

`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:doc-gq-join-bind"
```

Reconnecting to a different GameApi binds a new session to the Actor with the same id. Binding is
covered by [Session and Actor](24-actor-session.en.md).

## 5. Action Intake and the Send to the Owner

An action packet is relayed to the bound Actor and arrives at the Entry Spot's actor request handler.
The handler delegates to the application service and replies with the issued `eventId`.

<iframe class="zlink-diagram" src="/common/diagrams/sample-gamequest-progress-flow-en.html" title="Normal progress and completion" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-gamequest-progress-flow-en.html" target="_blank">↗ View larger</a></p>

`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:doc-gq-action-handler"
```

The service stores the event under the idempotency key and then sends it to the owner. A repeated
request with the same key yields the same `eventId`. If the owner is Unavailable the call fails as it
is — the framework does not resend to another node.

`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:doc-gq-store-dispatch"
```

The message to the owner is a one-way send whose Spot id is the `PlayerId`. If no Spot has that id,
this first message creates one on one of the eligible nodes.

`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:doc-gq-owner-send"
```

The reply means GameApi accepted the action, not that the owner has decided it. The decision is
confirmed by the push that follows. Calling an Instance Spot is covered by
[Spot](21-spot.en.md#6-the-other-kinds-of-spot), and the difference between send and request by
[Channel Messaging](20-channel-messaging.en.md#1-request-and-response).

## 6. The Owner Spot's Decision and Recording

When the Spot is created, its initialize callback records the `PlayerId` and the generation.

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-spot-init"
```

The gameplay message arrives at the Spot's packet handler.

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-apply-handler"
```

The processor reads the (`PlayerId`, `QuestId`) stream, restores the aggregate, evaluates the
condition, appends the events and updates the projection. A source event already stored is not
appended again.

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-process"
```

Messages of the same player are processed one at a time inside one Spot's turn, so there is no
competing writer between the fold and the append. The serialization boundary is covered by
[The Execution Model](32-execution-model.en.md).

## 7. The Progress Push — from the Owner to the Session Actor

Once the append completes, QuestMission sends the progress and completion messages with an Actor
direct send addressed by `PlayerId`. Which GameApi holds that Actor is resolved by the framework.

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-notify-actor"
```

GameApi's Entry Spot handler receives it and pushes a notify to the Actor's bound session.

`Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/GameApi/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/gameapi/Program.kt:doc-gq-progress-push"
```

A notify while no session is bound is not a success condition. The state lives in the event store,
and a reconnected client restores it with a query. Calling an Actor by id is covered by
[Actor](22-actor.en.md#3-the-calling-side--the-node-that-calls-an-actor).

## 8. Reconciliation and Closing

When a progress message is lost and the fact diverges from the fold, the client or an operator trigger
sends a sync request. The owner Spot reads the authoritative snapshot and appends a reconciliation
event through the same decision path.

<iframe class="zlink-diagram" src="/common/diagrams/sample-gamequest-reconcile-flow-en.html" title="Reset/reconcile and the failure boundary" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-gamequest-reconcile-flow-en.html" target="_blank">↗ View larger</a></p>

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-sync"
```

A message with the same `PlayerId` after an explicit close creates a Spot of a new generation, which
replays the stream and continues.

`Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/GameQuest/Server/QuestMission/src/main/kotlin/systems/zlink/samples/kotlin/gamequest/server/questmission/Program.kt:doc-gq-close-handler"
```

If the Ready owner's process disappears, the request in flight ends as Unavailable, and the framework
does not create the same Spot on another node by itself. Close and generations are covered by
[Activation and Lifetime](34-activation-lifetime.en.md).

## 9. Running and Verifying

One runner starts the Redis container, the server processes and the client scenario together.

```bash
framework/languages/java/samples/kotlin/GameQuest/run_sample.sh
```

The client asserts the action replies, the progress and completion notifies, the query after
reconnecting and the reconciliation result. The checks and the exact log strings are set by the
[GameQuest scenario](../../../common/sample/event/gamequest.en.md#9-client-self-check).

## 10. Related Documents

- Comparison with the other samples and how to choose: [Picking a Sample](14-samples.en.md)
- Requirements, message contract and completion criteria:
  [GameQuest scenario](../../../common/sample/event/gamequest.en.md)
- The same owner Spot and event sourcing applied to a lossless domain:
  [Reading Along: ShoppingMall](54-shoppingmall.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
