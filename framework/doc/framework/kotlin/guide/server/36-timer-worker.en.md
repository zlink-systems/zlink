---
title: "Timers and Workers · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/36-timer-worker.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Timers and Workers

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Actor Membership](35-actor-membership.en.md) | [Next: Relocation](37-relocation.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/36-timer-worker.en.md) · [C#/.NET](../../../dotnet/guide/server/36-timer-worker.en.md) · [Java](../../../java/guide/server/36-timer-worker.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/36-timer-worker.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can run periodic work inside a Spot and move long-running work out of its Spot queue. The
    code in this chapter comes from the samples in the repository.

[The Execution Model](32-execution-model.en.md#1-the-queues-work-waits-in)
covers how a Spot queue runs one Spot's work in order. **A timer puts work into the Spot queue on
each period; a worker runs long work in an execution context outside the Spot queue.**

## 1. Timers — Periodic Execution

A timer registers a name, a period and a handler on the Spot context. The tick **enters that
Spot's execution queue**, so the handler touches Spot state directly. Registration returns a timer
handle, and that handle cancels it later.

The name is unique within one Spot, and a period of `0` or less is a configuration error at
registration.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/handlers/TicTacToeGameTimerHandler.kt:doc-timer-handler"
```

### 1.1 Ticks Past Their Scheduled Time

When work piles up in the Spot queue or a handler runs long, a tick runs later than scheduled. The
overrun policy decides what happens to the ticks that went by.

| Value | Past the scheduled time | How to choose |
| --- | --- | --- |
| `SkipLateTicks` (default) | Drops the ticks that went by and delivers **only the one for the current time** | When only the latest state matters — state broadcasts, expiry checks |
| `CatchUpBounded` | Delivers the ticks that went by **up to a limit** and drops the rest | When the count of ticks itself matters — accumulating regeneration, simulation steps |
| `DelayNextTick` | Keeps no fixed period and recalculates the next schedule as **the moment the last tick ended plus the period** | When a minimum gap between runs has to hold — polling an external API |

The limit applies to `CatchUpBounded` alone, defaults to `1`, and is a configuration error at
registration when it is `0` or less. The first two policies hold a fixed rate measured from the
moment the timer started, so **one late tick does not move the next tick's scheduled time.**

### 1.2 The Values a Tick Delivers

The tick a timer handler receives carries the delay against schedule and the number of ticks
skipped as fields.

| Field | Meaning |
| --- | --- |
| Name | The name given at registration |
| Scheduled index · delivery index | Which scheduled tick this is and its actual delivery order. The difference is how many ticks have been dropped so far |
| Scheduled time · start time | The scheduled moment and the moment execution actually started |
| Delay | Start time minus scheduled time — this tick's delay against schedule |
| Skipped ticks | How many ticks were dropped immediately before this one |
| Period | The period registered |

A growing delay is the signal that the Spot queue is backing up. A handler that reads the value
and reports the load gives operations a place to look for the cause.

<iframe class="zlink-diagram" src="/common/diagrams/36-timer-worker-en.html" title="A timer enters the Spot queue; a worker uses another execution context" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/36-timer-worker-en.html" target="_blank">↗ View larger</a></p>

## 2. Workers — Running Outside the Spot Queue

A Spot queue runs one item at a time. Awaiting a heavy computation or external I/O
inside a handler **stops every other piece of work in that Spot meanwhile.** Such work is handed
to a worker call. A worker job bypasses the Spot queue and runs in another execution context, so it
does not occupy the Spot's turn.

Which call to use depends on whether the work is **synchronous code that occupies a thread** or
**asynchronous code that awaits completion.**

| | CPU worker | I/O worker |
| --- | --- | --- |
| What is handed over | A synchronous computation | An asynchronous call |
| Where it is used | Work that keeps the CPU busy — serialization, compression, pathfinding, image processing | Work that awaits a response — a database, a file, HTTP |

The worker thread pool itself — minimum and maximum threads, idle time, queue length — is set in
the root options.

## 3. The Terminator That Gives the Turn Back

Which terminator closes a worker call decides **whether the Spot's turn is held while waiting.**

| Terminator | The Spot's turn | Where it is used |
| --- | --- | --- |
| `Yield` | **Given back** while waiting | The default choice. Other work in the same Spot runs meanwhile |
| `Async` | **Held** while waiting | When the work is short and Spot state must not change meanwhile |
| `Submit` | Returns at once | When the result is not awaited and the work is only submitted |

For the difference between `Yield` and `Async`, checking state after a `Yield`, copying values
before it, and the Spots where it is available, see
[The Execution Model §5](32-execution-model.en.md#5-serial-execution-and-thread-occupancy).

## 4. Related Documents

- What runs in order in a Spot queue — [The Execution Model](32-execution-model.en.md)
- When arrival outpaces processing — [Backpressure](33-backpressure.en.md)
- What happens to a timer during relocation — [Relocation](37-relocation.en.md)
- The exact option names and defaults — the [16. Options](16-options.en.md) for your language

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
