---
title: "How Session Binding Works · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/39-session-binding.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# How Session Binding Works

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: How STREAM Works](38-stream-boundary.en.md) | [Next: 12. Operations — Runtime Metrics · Graceful Drain · Readiness](12-operations.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/39-session-binding.en.md) · [C#/.NET](../../../dotnet/guide/server/39-session-binding.en.md) · [Java](../../../java/guide/server/39-session-binding.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/39-session-binding.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can tell how many bindings one connection may hold, what is refreshed when a bound
    target moves, and who is notified when the connection drops.

[Session and Actor](24-actor-session.en.md) went as far as binding one connection to one Actor and
receiving a push. This chapter covers the rules that binding keeps.

## 1. How Many May Be Bound — Several per Session, One per Actor

<iframe class="zlink-diagram" src="/common/diagrams/39-binding-shape-en.html" title="A session binds several; an Actor binds to one" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/39-binding-shape-en.html" target="_blank">↗ View larger</a></p>

**One session may bind several Actors at once.** A single connection may use a player Actor and a
party Actor together.

**One Actor, in return, is bound to exactly one session at a time.** When a new binding is
committed, the previous one becomes void and a late message arriving on it is refused.

On a session with several bindings, each packet names its target Actor. When the client sends through
an Actor handle, the packet carries that Actor's slot and the session hands it to the dispatch
context's Actor ([Session and Actor Connection §3.2](24-actor-session.en.md#32-forwarding-what-is-left)).
**The framework does not pick an arbitrary Actor** — a packet with a slot that is not a current
binding isn't handed to the session; it is refused.

The binding call treats a duplicate bind as an error. A flow that may already be bound, such as a
resent authentication, uses the find-or-bind call instead.

## 2. Binding and Spot Membership — Independent of Each Other

Binding is separate from the Actor's Spot membership. Even when the Actor moves to another Spot or
another node, the actor id and [generation](22-actor.en.md#33-generation-in-a-reference) are kept and the framework refreshes the
binding's route — [Actor Membership](35-actor-membership.en.md) and
[Relocation](37-relocation.en.md) cover those moves.

**The relay does not look the record up again.** The session keeps, per Actor, the route it
verified at bind time and sends on that. When the Actor moves, the framework refreshes that kept
route once the move is committed — the application does not bind again.

Once bound, all an Actor can do on that connection is push and disconnect. The answer to a request
is handled by the request handler's return value.

## 3. Notification When the Connection Drops

When the connection drops physically, the framework notifies every binding held at that moment
automatically. The application calls explicitly only to announce a logical disconnect while the
connection is still up.

A disconnect neither deletes the Actor nor moves it to the Entry Spot. A session that reconnects can
look the same reference up again and [bind it again](24-actor-session.en.md#31-binding). TicTacToe's
game Spot marks the disconnected Actor but leaves the room and match state in place.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/TicTacToeGame.kt:doc-disconnect-actor"
```

This call is used only to report an application-level disconnection while the physical connection remains open.

**Run result.** On 2026-09-22, running .NET TicTacToe's `run_sample.sh` and closing the host
client left this real record for the connection that held one bound Actor in `play-a.log`.

```text
20:12:39.106 info: TicTacToe.Server.Play.Infrastructure.ZLink.Sessions.PlaySession[0] client -> play stream: disconnected. sessionId=00000002, actors=1
```

**A failed notification for one Actor does not stop the rest.** The framework fixes the list of
bindings held when the connection dropped and notifies each Actor; one of them failing, or a
callback overrunning its deadline, does not stop the remaining notifications or the session cleanup.

**An automatic notification overlapping an explicit call still runs the callback once.** The
framework merges the two notifications for the same binding, so a drop right after an explicit call
does not run the Spot's disconnect callback twice. Call the Actor directly only when the connection
is still up but the application protocol treats it as disconnected.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt:session-disconnect-notify"
```

## 4. When a Bind Fails or Becomes Void

| Situation | Result |
| --- | --- |
| The Actor does not exist, or is not in a state to receive | The bind ends with a typed error |
| The reference's [generation](22-actor.en.md#33-generation-in-a-reference) differs | A stale reference is not bound to another generation |
| The Actor is moving | It ends as a moving error and is not retried silently |
| The Actor moved after the bind | The framework refreshes the route; the session is not bound again |
| The session dropped | The Actor and its Spot membership are kept |
| A reply arriving after the session closed | It is dropped, not used as a reply for a new session or a new binding |
| A timeout or a route failure after the relay | Nothing is resent automatically to another Actor, a new owner or another node |

The mesh name and the node identifier a reference carries are **the values from the first lookup**.
The application does not assemble a stale route itself; it obtains the current reference again
through the manager's lookup call —
[Location](25-location.en.md#5-the-scope-of-a-lookup-result--a-node-name-is-not-a-call-address)
covers that boundary.

## 5. Related Documents

- The steps up to binding — [Session and Actor](24-actor-session.en.md)
- Where the connection is accepted — [STREAM](23-stream.en.md) · [How STREAM Works](38-stream-boundary.en.md)
- What gets bound — [Actor](22-actor.en.md)
- What happens during a move — [Relocation](37-relocation.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
