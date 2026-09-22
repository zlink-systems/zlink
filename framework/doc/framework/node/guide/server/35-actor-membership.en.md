---
title: "Actor Membership · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/35-actor-membership.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Actor Membership

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Activation and Lifetime](34-activation-lifetime.en.md) | [Next: Timers and Workers](36-timer-worker.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/35-actor-membership.en.md) · [C#/.NET](../../../dotnet/guide/server/35-actor-membership.en.md) · [Java](../../../java/guide/server/35-actor-membership.en.md) · [Kotlin](../../../kotlin/guide/server/35-actor-membership.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

The code in this chapter comes from the [`TicTacToe` sample README](https://github.com/zlink-systems/zlink-node-examples/blob/main/samples/TicTacToe/README.md). Download the [examples repository](https://github.com/zlink-systems/zlink-node-examples/blob/main/samples/TicTacToe/README.md) and follow its README's Download, Build and Run sections to reproduce the results below.

!!! info "What you get from this chapter"

    You can move an Actor between Spots and let the receiving side accept or refuse that move. The
    code in this chapter comes from the samples in the repository.

An [Actor](22-actor.en.md) is always inside some Spot, and right after creation it is in an Entry
Spot. This chapter covers **the procedure that moves it into a room** — who admits it, when it
runs, and what stops the move.

## 1. What the Move Targets — Only the Execution Site

An Actor entering a room means **the Spot its callbacks run in changes.** The Actor itself stays,
and so does its id. Leaving the room returns it to the Entry Spot.

The receiving Spot admits or refuses first; once membership is committed the receiving side is
notified.

| Step | Where | What it decides |
| --- | --- | --- |
| Admission | `onActorJoin` on the User Spot being entered | Whether to accept, and what answer to return with it |
| Commit | The Framework | Changes the location record and the membership together |
| Notice | `onJoinedActor` on the receiving side, `onLeaveActor` on the departing side | Runs after the commit |

The way back to the Entry Spot has no admission step, because that is the default membership —
[Activation and Lifetime](34-activation-lifetime.en.md) covers the callbacks per kind.

<iframe class="zlink-diagram" src="/common/diagrams/35-actor-join-en.html" title="A reservation is the Defer() call; the join starts after the handler ends" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/35-actor-join-en.html" target="_blank">↗ View larger</a></p>

## 2. Reserving a Join — It Runs After the Handler Ends

A join call has no form that awaits the result on the spot. **It only reserves, and the handler
ends.** `defer()` registers "start this join when this handler ends" — that is why this chapter
calls it a reservation. The reservation runs after the handler completes normally.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts:doc-join-defer"
```

### 2.1 Why No Awaiting Form Is Offered

A join changes that Actor's location and membership. When the owner of the target Spot is another
node the same operation also relocates the Actor, so it includes a location lookup, the receiving
side's admission, and the commit of the record.

**Awaiting that completion inside the current turn blocks the Actor against itself.** An Actor runs
the work in its queue one item at a time. If the running handler awaits the join, the join can
only proceed once that handler ends, so each waits for the other.

### 2.2 How the Handler Ends After Reserving

What becomes of the reservation is decided by how the handler ends.

| Handler ending | The reserved join |
| --- | --- |
| Normal completion | Becomes active and starts running |
| An exception, a cancellation, or a failure encoding the reply | Discarded. The join never starts |

A reservation can be made **only while the current handler's registration scope is open.** Calling
after the handler ends, or from a background task detached from it, is `InvalidOperation`.

| Can be called from | Cannot be called from |
| --- | --- |
| An Actor's send and request handlers | Factories and the configuration step |
| A User or Entry Spot's packet, request, subscription and timer handlers | Lifecycle callbacks |
| | Relocation adapters |
| | Instance Spot handlers |
| | A background task detached from a handler |

!!! warning "A detached task is not guaranteed to be caught"

    It may go undetected before the handler ends. Do not call from the places in the right-hand
    column in the first place.

Reserving twice in the same call is `InvalidOperation`, and another membership transition already
pending for that Actor is `Unavailable`. **An Actor that already belongs to the Spot joining that
same Spot** succeeds without moving — it touches neither the record nor the membership, and runs
no admission, join or leave callback.

### 2.3 Where the Result Arrives

The result arrives on the Actor's join-completion callback. Its name in the tabs is
`OnJoinCompletedAsync` for C#/.NET, `on_join_completed` for C++, and `onJoinCompleted` for Java,
Kotlin, and Node/TypeScript.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Actors/play-actor.ts:doc-join-completed"
```

**Which Actor runs that callback is decided by the result.**

| Result | The Actor that runs it |
| --- | --- |
| Accepted | The **destination** Actor that committed the move |
| Rejected, or failed before the commit | The **origin** Actor, which stayed put |

When a join to a Spot on another node succeeds, the completion goes to the Actor on the destination
node. That is why receiving the result inside the handler run where the application registered the
join is not a shape that holds — the Actor that handler ran on is being torn down by then.

Once a reservation is active, ordinary messages that arrive afterwards do not run ahead of the
completion callback. Ordinary processing for that Actor waits until the join finishes.

The completion callback also carries an id that distinguishes a retried result.

Returning from a User Spot to the Entry Spot works the same way.

## 3. The Join Timeout

The default timeout is 5 seconds; a given value has to be finite and positive. A value out of
that range ends in an error at the registration. A join request and reply have no size limit of
their own — a request and reply that cross to another node follow the same wire limit as any
other message.

## 4. The Limit on Requests to a Reserved Actor

Sending a request from **the same handler to the Actor that holds the reservation, and awaiting the
reply, is a circular wait.** The request waits behind the reservation, the reservation opens only
when this handler ends, and the handler cannot end while it awaits the reply.

The Framework refuses that request with `InvalidOperation` **before submitting it.** It ends in an
error rather than hanging, so on seeing it, check whether the reservation and the request target
the same Actor.

## 5. When a Reservation Does Not Survive

A reservation lives **only in the memory of the current process.** If the process goes down before
the join runs or reaches the record, that reservation is not replayed. The Actor's location and
membership stay as they were — nothing is left half-moved.

When it overlaps relocation or shutdown, **whichever commits first wins.**

| What committed first | Result |
| --- | --- |
| The join | The maintenance procedure waits for the join to finish |
| Relocation's seal | The join ends as `Unavailable` |
| Shutdown's seal | The join ends as `ShuttingDown` |

## 6. Sending to an Actor Inside a User Spot

A message is sent by actor id without knowing which Spot or node the Actor is on. A packet
addressed to an Actor inside a room is received by that room's handler.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler.ts:doc-actor-packet-handler"
```

This handler receives the room and the Actor together. When the room's execution mode is
`SpotWide`, the room's state and the Actor's state are handled within one turn —
[The Execution Model](32-execution-model.en.md) covers that boundary.

### 6.1 Messages Sent While the Actor Is Moving

While an Actor moves to another node, the sender still names only the actor id. On every call the
framework looks up the **current owner** in the record again and sends to that node.

A message sent to the previous owner by a caller still holding the pre-move location is not dropped
either. **The previous owner that receives it hands it on to the new owner** — not by telling the
sender a new address to retry against, but by the receiving node passing it along. That hand-off has
a validity window; a message arriving after it is treated as the ordinary stale-route failure. The
application does not track node identifiers.

**A request sent during the move also completes at the original caller.** The reply the destination
produces is correlated back to the original caller, the timeout follows the caller's existing path,
and a late reply is dropped. The number of requests awaiting a reply during a move is observed
through a runtime metric — [Operations and Lifecycle](12-operations.en.md#1-runtime-metrics) is that
place.

## 7. Related Documents

- The entity called by id — [Actor](22-actor.en.md)
- The place it moves into — [Spot](21-spot.en.md)
- Membership callbacks per kind — [Activation and Lifetime](34-activation-lifetime.en.md)
- The other procedure that moves the execution site — [Relocation](37-relocation.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
