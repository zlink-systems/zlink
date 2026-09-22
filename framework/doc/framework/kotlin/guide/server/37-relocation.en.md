---
title: "Relocation · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/37-relocation.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Relocation

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Timers and Workers](36-timer-worker.en.md) | [Next: How Channels Work](30-channel-patterns.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/37-relocation.en.md) · [C#/.NET](../../../dotnet/guide/server/37-relocation.en.md) · [Java](../../../java/guide/server/37-relocation.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/37-relocation.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can move live Spots and Actors to another node, and tell what survives the move and what
    the application has to hand over itself. The code in this chapter comes from the samples in the
    repository.

[Spot](21-spot.en.md) and [Actor](22-actor.en.md) were covered as running on the node that created
them. In operations the time comes to take that node down. **Relocation is the procedure that
moves the execution site while leaving the logical id alone**, and this chapter covers that
procedure and the part the application owns.

## 1. What Survives the Move

Nothing the calling side was using changes.

| What survives | Meaning |
| --- | --- |
| Spot id, actor id, and [generation](22-actor.en.md#33-generation-in-a-reference) | The logical id the caller was using is unchanged. No address has to be announced again |
| Messages not yet run and the accepted journal | Work still in the queue at the seal is resumed at the destination |
| Timer registrations and pending ticks | Names, periods, options and the cursor move together, so nothing is registered again at the destination |
| The route of a bound STREAM session | The client connection stays and the route is pointed at the new owner |
| Application state | Captured and restored by the adapter registered on the factory — this part alone belongs to the application |

**State in transit does not pass through the Location Store.** The departing node sends it to the
destination node directly over the mesh connection.

<iframe class="zlink-diagram" src="/common/diagrams/37-relocation-move-en.html" title="The logical id stays; only the execution site moves" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/37-relocation-move-en.html" target="_blank">↗ View larger</a></p>

## 2. The Application's Part — the Adapter

To move an Actor or Spot to another node, the application state held by its instance (fields on the
user class) must be serialized to bytes, sent, and restored into a new instance at the destination.
The Framework does not know the state of a user class, so the application provides that serialization
and restoration as a relocation adapter. Implement a class that captures state to bytes and restores
bytes into a new instance, then choose it with
[`preserveStateWith`](21-spot.en.md#33-registration) when registering the factory. Location
authority, queued messages, timers, the accepted journal, and session routes move with the Framework,
not in the adapter.

**What goes in it.** Keep only state needed to restore the new instance; leave out values derived
from it and caches that can be rebuilt.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.kt:doc-relocation-adapter"
```

The adapter can be called again within the same move. **Capturing and restoring must produce the
same result when run twice.** To hold the handed-over bytes beyond the callback, copy them.

### 2.1 The Relocation Policy the Factory Registration Picks

The policy is fixed at factory registration and does not change while running. It applies both when
an Actor joins a Spot on another node and when a node is drained in operations.

| Policy | What the destination gets |
| --- | --- |
| No relocation | Refuses before a move to another node begins. While such targets remain, draining a node cannot finish |
| Recreate | Builds a new instance under the same logical id. Pending messages and timers are kept; application state is not restored |
| Preserve state with an adapter | Restores the bytes the adapter captured into the new instance. The queue and the timers are kept as well |

The registration call is named per language — [the Spot registration that chooses
`preserveStateWith`](21-spot.en.md#33-registration) is that place.

## 3. When State Is Captured — the Factory Registration Decides

One handler invocation or one tick is a [turn](32-execution-model.en.md#1-the-queues-work-waits-in),
and the next turn starts only when that turn ends. The Framework never interrupts a running turn,
so it can capture state only between turns. **Who decides the moment of capture** is chosen at
factory registration.

| Mode | Who decides | Where it is used |
| --- | --- | --- |
| Framework-managed (default) | The instant the current turn ends after a move request | A Spot where one message is one state change (a chat room) |
| Application-signaled | The instant the turn that called `RelocationReady().Defer()` ends | A Spot whose unit spans several turns (an FPS round) |

**Default mode.** When a move request arrives, the Framework calls the adapter immediately after
the current turn ends. For a Spot where one message is one state change, such as a chat room, the
state in that gap is always whole.

**Application-signaled mode.** An FPS round may consist of a start tick, many input packets, and a
settlement tick, so its state between those turns is a half-finished round. Choose this mode when
registering the factory, then call `RelocationReady().Defer()` in the handler that closes the unit.
It says, "after this turn ends, it is safe to capture." New turns keep running, and state keeps
changing, until the signalled turn ends.

<iframe class="zlink-diagram" src="/common/diagrams/37-relocation-capture-en.html" title="When state is captured after a move request" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/37-relocation-capture-en.html" target="_blank">↗ View larger</a></p>

Bingo sends this signal in the turn that ends a round.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/Bingo/Server/Play/src/main/kotlin/systems/zlink/samples/kotlin/bingo/server/play/infrastructure/zlink/spots/bingoroomspot/BingoRoomSpot.kt:doc-relocation-ready"
```

The application-signaled mode is available in a `SpotWide` User Spot only —
[The Execution Model](32-execution-model.en.md) covers that boundary.

## 4. The Unit the Execution Mode Decides

| Execution mode | The unit that moves |
| --- | --- |
| `SpotWide` User Spot | The Spot and its Actors move **together as one unit** |
| Entry Spot, `PerActor` User Spot | Each Actor moves on its own |

**This move calls no join or leave callback.** Membership is left as it is and only the execution
site changes, so to the application it is neither an arrival nor a departure —
[Actor Membership](35-actor-membership.en.md) covers those events.

## 5. The Procedure and the Point of No Return

1. The objects to move and the eligibility and headroom of the destination candidates are checked
   first. With nowhere to receive them it ends as blocked, leaving the departing side untouched.
2. The host is published as relocating and a notice is queued on each execution queue.
3. When the notice reaches a turn boundary, only the running turn finishes and no new turn starts.
   Receiving stays open and later messages are held on the departing side.
4. Messages left at the seal, the accepted journal, timer registrations and pending ticks, and the
   captured state are sent straight to the destination.
5. Authority and membership are changed.
6. Once the destination reports that it is ready to receive, the held messages are relayed and the
   cutover is instructed. The destination merges the backlog in order, finishes the required
   callbacks and opens dispatch.
7. When dispatch has finished for every unit the host becomes relocated. Connections and
   infrastructure are kept until the shutdown call.

**A failure before the first commit can restore the departing side's queue and admission.** After
the first commit there is no rollback; recovery continues on the destination, and passing the
deadline ends it as force-stopped.

## 6. Related Documents

- What moves — [Spot](21-spot.en.md) · [Actor](22-actor.en.md)
- What decides the unit — [The Execution Model](32-execution-model.en.md)
- Arrivals and departures — [Actor Membership](35-actor-membership.en.md)
- Why timers move along — [Timers and Workers](36-timer-worker.en.md)
- The call operations makes — [12-operations](12-operations.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
