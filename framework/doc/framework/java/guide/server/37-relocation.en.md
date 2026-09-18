---
title: "Relocation · Java"
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
View in another language — [C++](../../../cpp/guide/server/37-relocation.en.md) · [C#/.NET](../../../dotnet/guide/server/37-relocation.en.md) · **Java** · [Kotlin](../../../kotlin/guide/server/37-relocation.en.md) · [Node/TypeScript](../../../node/guide/server/37-relocation.en.md)
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
| Spot id, actor id and the generation value | The logical id the caller was using is unchanged. No address has to be announced again |
| Messages not yet run and the accepted journal | Work still in the queue at the seal is resumed at the destination |
| Timer registrations and pending ticks | Names, periods, options and the cursor move together, so nothing is registered again at the destination |
| The route of a bound STREAM session | The client connection stays and the route is pointed at the new owner |
| Application state | Captured and restored by the adapter registered on the factory — this part alone belongs to the application |

**State in transit does not pass through the Location Store.** The departing node sends it to the
destination node directly over the mesh connection.

<iframe class="zlink-diagram" src="/common/diagrams/37-relocation-move-en.html" title="The logical id stays; only the execution site moves" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/37-relocation-move-en.html" target="_blank">↗ View larger</a></p>

## 2. The Application's Part — the Adapter

An adapter captures and restores **application state only**, as bytes. Location authority, queues,
timers, the accepted journal and session routes are handled by the Framework.

```java
--8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.java:doc-relocation-adapter"
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

The registration call is named per language — the registration in [Spot](21-spot.en.md) is that
place.

## 3. When State Is Captured — the Factory Registration Decides

The Framework stops accepting new turns on the departing node, captures state through the adapter,
restores it on the destination node and then hands authority over. **Who decides the moment of
capture** is chosen at factory registration.

| Mode | Who decides | Where it is used |
| --- | --- | --- |
| Framework-managed (default) | The Framework — between a finished turn and the next | Most Spots |
| Application-signaled | The application — at the end of the turn that signaled | A Spot whose unit of consistency spans several turns |

**When the default mode holds.** The Framework never interrupts a running turn. A handler or a tick
is captured only after it finishes, so when a state change completes within one turn, state
captured at a turn boundary is always consistent.

**When it does not hold.** If the unit of consistency spans several turns, state captured at a turn
boundary may be incomplete. A round in a shooter is such a case — a start tick, many input packets
and a settlement tick, where restoring the intermediate state cannot continue the round.
**The Framework knows turn boundaries but not the unit of consistency the application defined.**

Register the application-signaled mode and the Framework does not capture on its own; it waits for
the signaled moment. The mode is available in a `SpotWide` User Spot only —
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
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
