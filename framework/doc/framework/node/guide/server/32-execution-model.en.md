---
title: "The Execution Model · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/32-execution-model.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# The Execution Model

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Monitoring](26-monitoring.en.md) | [Next: Backpressure — When Arrival Outpaces Processing](33-backpressure.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/32-execution-model.en.md) · [C#/.NET](../../../dotnet/guide/server/32-execution-model.en.md) · [Java](../../../java/guide/server/32-execution-model.en.md) · [Kotlin](../../../kotlin/guide/server/32-execution-model.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can tell what runs together inside one Spot, what queues up behind it, and what decides
    that boundary. The code in this chapter comes from the samples in the repository.

[Spot](21-spot.en.md) and [Actor](22-actor.en.md) summarized it as "work addressed to it is
handled one at a time." This chapter covers how far that sentence holds — which work shares one
line, which work is split, and what decides the boundary.

## 1. The Queues Work Waits In

Work arriving at a Spot waits in two queues. Packets addressed to the Spot itself and timers go
into the **Spot queue**; payloads addressed to an Actor that belongs to the Spot go into the
**Actor queue**.

**A business message addressed to an Actor does not pass through the Spot queue.** No Spot
callback receives it and hands it on; it enters the Actor queue from the start.

| Queue | Goes in | Does not go in |
| --- | --- | --- |
| Spot queue | Payloads addressed to the Spot, matching Logical Multicast payloads, timer callbacks, Actor join and leave, lifecycle callbacks | **Actor business payloads** |
| An Instance Spot's queue | Payloads addressed to the Spot and timer callbacks | Anything about Actors. **Rejected at registration** |
| Actor queue | Actor business payloads | — |

Registering Actor membership or a Logical Multicast subscription on an Instance Spot is rejected
**when it is registered or when the Spot is prepared**, not while it runs.

## 2. What Decides the Serialization Boundary

Whether work from different queues may run at the same time is decided by **the kind of Spot and
its execution mode.**

| | Serialization boundary | State ownership |
| --- | --- | --- |
| Entry Spot | Serializes the Spot queue and the Actor queue separately. Different queues may run at the same time | Each Actor owns its own. State shared between Actors belongs in an external store |
| User Spot, `SpotWide` (default) | Serializes Spot handlers, member Actor handlers, timers and lifecycle callbacks through one common gate | The Spot instance owns it. State shared with Actors needs no separate synchronization |
| User Spot, `PerActor` | Serializes per Actor and per Spot lane. Different lanes may run at the same time | Each Actor owns its own. State shared between lanes belongs in an external store |
| Instance Spot | Serializes the Spot queue's handlers and timers. There is no Actor queue | The Spot instance owns it |

<iframe class="zlink-diagram" src="/common/diagrams/06-spot-en.html" title="Spot execution model — SpotWide and PerActor" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/06-spot-en.html" target="_blank">↗ View larger</a></p>

The execution mode is fixed when the factory is registered and is not changed while running.

## 3. SpotWide — Execution Through One Gate

`SpotWide` is the default. Every callback bound for that Spot — another Actor's message, a timer,
a lifecycle callback — passes one common gate and runs one turn at a time in a single lane.

<iframe class="zlink-diagram" src="/common/diagrams/06-spotwide-lockfree-en.html" title="SpotWide — lock-free sequential execution" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/06-spotwide-lockfree-en.html" target="_blank">↗ View larger</a></p>

No two turns run at the same instant, so **a handler touches the state of the Spot and of its
member Actors directly, in ordinary code, without locks.** In relocation the Spot and its Actors
move together as one unit. In exchange, one long callback delays every following callback of that
Spot.

Being the default, it can be left out; writing it down makes the mode the Spot runs in visible at
the registration.

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/bingo-play-module.ts:doc-execution-mode"
```

## 4. PerActor — One Lane Each

`PerActor` is chosen when throughput requires each Actor to run independently. The Spot itself is
used as a shell that holds no state.

Different lanes run at the same time, so **state that several Actors change together, and
schedules kept per Spot, belong in an external store such as Redis or a database.** The factory's
relocation policy can only be `RecreateOnRelocation`.

**An Entry Spot follows the same model.** Its Spot queue and Actor queue are separate, so it
carries the same constraints.

## 5. Serial Execution and Thread Occupancy

Serial execution does not mean holding one thread throughout. When a handler reaches a suspension
point the executing thread handles other work, but **the turn is held until the handler
completes.** Under `SpotWide` the next callback of the same Spot does not start in the meantime.

When the next turn has to run while a long I/O is awaited, use the `Yield` contract in
[Timers and Workers](36-timer-worker.en.md).

## 6. Related Documents

- State objects called by id — [Spot](21-spot.en.md) · [Actor](22-actor.en.md)
- Creation points and lifecycle callbacks per kind — [Activation and Lifetime](34-activation-lifetime.en.md)
- The contract that yields a turn — [Timers and Workers](36-timer-worker.en.md)
- When arrival outpaces processing — [Backpressure](33-backpressure.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
