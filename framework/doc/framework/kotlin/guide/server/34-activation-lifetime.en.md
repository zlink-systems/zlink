---
title: "Activation and Lifetime · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/34-activation-lifetime.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Activation and Lifetime

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Backpressure — When Arrival Outpaces Processing](33-backpressure.en.md) | [Next: Actor Membership](35-actor-membership.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/34-activation-lifetime.en.md) · [C#/.NET](../../../dotnet/guide/server/34-activation-lifetime.en.md) · [Java](../../../java/guide/server/34-activation-lifetime.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/34-activation-lifetime.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

This chapter quotes code from the `TicTacToe` sample's `Server` directory. Bootstrap and build that language's sample tree to inspect the activation and lifetime examples below in running code.

!!! info "What you get from this chapter"

    You can tell when each of the three kinds of Spot is created, which callbacks it receives, and
    how long a service injected into it lives. The code in this chapter comes from the samples in
    the repository.

What [Spot](21-spot.en.md) created was **the Spot an application creates explicitly**. This chapter
covers how the other two kinds differ, which lifecycle callbacks each kind receives, and the
injection scope that lasts as long as the Spot.

## 1. The Kinds of Spot

All three carry an id and state and run their callbacks in order. They differ in when they are
created, in Actor membership, and in how they are closed.

| | Entry Spot | User Spot | Instance Spot |
| --- | --- | --- | --- |
| Created | By the Framework when the Object Server starts | Explicitly by the application through the spot manager | When the first message arrives for that id |
| Spot id | Issued by the Framework | Issued by the Framework on create; named by the caller on get-or-create | Named by the caller as the message's target id |
| Stable type | Not registered | Required | Required |
| Actor membership | Supported. The default place an Actor runs right after creation | Supported. Actors move in and out by join and leave | Not supported |
| Closing from the application | Not offered | Through a close call or from its own context | From its own handler or timer context |
| Where it is used | The default place for an Actor that belongs to no User Spot yet | Rooms, stages, zones | A unit that handles requests per id, such as a matchmaking worker |

<iframe class="zlink-diagram" src="/common/diagrams/34-spot-kinds-en.html" title="Three kinds of Spot — what creates them" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/34-spot-kinds-en.html" target="_blank">↗ View larger</a></p>

An Instance Spot has no create call of its own. Name the instance type on the first message and
the Framework selects an existing instance or creates one where it is needed, then **handles that
same message.**

## 2. Lifecycle Callbacks per Kind

The names follow each language; the conditions and the order are the same.

| Callback | Entry | User | Instance | When |
| --- | :---: | :---: | :---: | --- |
| `configure` | O | O | O | The configuration step where handlers are registered |
| `onCreate` | X | O | X | Examines a request to create a new User Spot and decides whether to accept it. Not called when an existing Spot was found |
| `onInitialize` | O | O | O | Initialization of the created instance. An Instance Spot receives only this, with no `onCreate` |
| `onClosing` | O | O | O | Before a still-valid local instance is torn down |
| `onActorJoin` | X | O※ | X | Accepts or refuses an existing Actor that wants to enter this User Spot |
| `onCreateActor` | O※ | X | X | Accepts or refuses a new Actor's first Entry Spot membership |
| `onJoinedActor` | O※ | O※ | X | Tells the **receiving** Spot that the join commit finished |
| `onLeaveActor` | O※ | O※ | X | Tells the **departing** Spot after the commit. It does not mean the Actor is gone |
| `onDisconnectActor` | O※ | O※ | X | When the connection of an Actor in that Spot drops |

※ Only for a Spot that names an actor type and so supports Actor membership.

**Membership callbacks are split between the departing Spot and the receiving Spot.** So when an
Actor in a User Spot returns to its Entry Spot, **the Entry Spot's `onCreateActor` and
`onActorJoin` are not called** — returning to the Entry Spot is the default membership and has no
admission step. In both directions only the receiving side's `onJoinedActor` and the departing
side's `onLeaveActor` run after the commit.

!!! info "Relocation is not an arrival or a departure"

    When relocation restores an Actor into another node's Entry Spot these callbacks are not
    called. Membership is left as it is and only the execution site moves — covered by
    [Relocation](37-relocation.en.md).

## 3. Entry Spot — Created by the Framework

There is one per Object Server. It accepts or refuses Actor creation requests and handles the
lifecycle of Actors arriving and leaving.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.kt:doc-entry-spot"
```

### 3.1 What an Entry Spot Does Not Hold

**Keep no per-Actor state in an Entry Spot.** An Actor's state belongs to the Actor, and the Entry
Spot provides only handlers and membership callbacks. There is one Entry Spot per Object Server, so
per-Actor values accumulated here grow with the number of Actors that Object Server serves.

### 3.2 Actor Creation and Destruction Happen in the Entry Spot

The Entry Spot accepts or refuses Actor creation requests. Its context also provides the call that
destroys an Actor. An Actor in a User Spot first returns to the Entry Spot before it is destroyed;
[Actor Membership](35-actor-membership.en.md) handles that move.

The destroy call takes the current instance and does not run membership callbacks again. It clears
the Framework's registration record and the bound session route.

## 4. User Spot — Created by the Application

It is created by naming a stable type, and the id that comes back is the address for every later
call. Refuse in the create callback and the call fails, leaving no Spot behind.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/api/handlers/CreateGameHttpHandler.kt:doc-create"
```

## 5. The Lifetime of an Injected Service

When the Framework activates a Spot it creates one injection scope and resolves the dependencies
of the Spot itself and of its handlers from that scope. The scope is torn down when the Spot
closes or moves to another node. So **a service registered as scoped is one instance for as long
as that Spot lives.** That differs from one created per HTTP request.

Actor handlers use a separate Actor activation scope. Different Actors share neither handlers nor
scoped dependencies. When an Actor leaves, is destroyed, or moves, the source scope is torn down
and a new one is created at the destination.

**Injecting an ORM context into the constructor of a Spot or a Spot handler causes trouble.** A
room that lives for hours keeps that context alive for hours.

| Symptom | What happens |
| --- | --- |
| Growing memory | The change tracker keeps tracking every entity it has read |
| Stale reads | Reading the same key again returns the tracked earlier instance |
| A stuck error state | A failed save leaves the context soiled for the rest of the Spot's life |

Registering the handler type as transient or singleton does not change the lifetime the Framework
sets, because the Framework creates the handler and resolves only its dependencies from the
activation scope.

**The first choice is not to reach a store from a Spot at all.** Saving and reading are requested
from a service that owns a channel handler, and the Spot owns only in-memory state and execution
order. A channel handler has a scope per dispatch, so it may take an ORM in its constructor — the
handler in [Channel Messaging](20-channel-messaging.en.md) is that place.

## 6. Related Documents

- State objects called by id — [Spot](21-spot.en.md) · [Actor](22-actor.en.md)
- What runs together — [The Execution Model](32-execution-model.en.md)
- The rules for moving an Actor between Spots — [Actor Membership](35-actor-membership.en.md)
- Moving the execution site — [Relocation](37-relocation.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
