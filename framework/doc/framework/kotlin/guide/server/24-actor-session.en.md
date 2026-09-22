---
title: "Session and Actor · Kotlin"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/24-actor-session.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Session and Actor

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: STREAM](23-stream.en.md) | [Next: Location](25-location.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/24-actor-session.en.md) · [C#/.NET](../../../dotnet/guide/server/24-actor-session.en.md) · [Java](../../../java/guide/server/24-actor-session.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/24-actor-session.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

This chapter quotes code from the tutorial's [`Server` and `StreamClient` directories and its Run section](https://github.com/zlink-systems/zlink-kotlin-examples/blob/main/tutorial/README.md#run); bootstrapping and building that tree reproduces the results below.

!!! info "What you get from this chapter"

    You can bind one external client's connection to one Actor, and have that Actor push a
    notification back over the same connection. The code in this chapter runs as it stands in
    `framework/languages/java/tutorial/kotlin`.

A [STREAM](23-stream.en.md) session ends when its connection drops. A player's state has to
outlive that, and what holds it is an [Actor](22-actor.en.md). **This chapter binds the two** —
once bound, packets the session does not handle reach that Actor, and the Actor can push over
that connection.

## 1. The Problem Binding Solves

A connection and an entity have different lifetimes. When the same player drops and returns the
connection is a new one and the entity is the one that was already there. So **the connection is
made to point at its entity**, and packets after that are received by the entity.

<iframe class="zlink-diagram" src="/common/diagrams/24-actor-session-binding-en.html" title="Binding one connection to one entity" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/24-actor-session-binding-en.html" target="_blank">↗ View larger</a></p>

The session does not disappear once bound. Packets the session knows are handled by the session
first, and **only the ones it does not know** go to the Actor.

## 2. Deciding the Packets

An authentication request, its answer, and the notification the Actor pushes. The last one answers no request.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/Shared/src/main/kotlin/systems/zlink/tutorial/shared/Contracts.kt:session-actor-contracts"
```

## 3. The Receiving Side — the Node That Binds the Connection

!!! note "The receiving process"

    The code in this section goes into the process that registered the stream node in
    [STREAM](23-stream.en.md). **Actor dispatch has to be turned on at that registration** for the
    relay in this chapter to work.

### 3.1 Binding

When the client names its id, the Actor for that id is found or created and bound to this connection.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/sessions/SessionHandlers.kt:session-actor-bind"
```

**A client that returns with the same id binds to the Actor that is already there.** That is why creating and finding are one call.

### 3.2 Forwarding What Is Left

Packets the session did not handle are sent to the bound Actor. Before the bind there is nowhere
to send them, which is where **authentication first** is enforced.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/sessions/GameSession.kt:session-actor-relay"
```

A forwarded packet is received by that Actor's handler — the same handler written in
[Actor](22-actor.en.md). One handler is called both by a mesh call and by a STREAM relay.

### 3.3 The Actor Pushes Over That Connection

An Actor knows the connection bound to it. This is not a reply but **a notification it sends on its own.**

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/actors/PlayerHandlers.kt:actor-push"
```

!!! note "A push without a bound connection ends with InvalidOperation"

    The same handler is also called by a mesh call, where no connection is bound. In that case, the
    push ends with `InvalidOperation` at the call's terminal, and the failure is observed through each
    language's asynchronous completion mechanism. The code above has already completed the rename
    before pushing, so it discards **only that failure**.

## 4. The Attaching Side — a Client Outside the Mesh

!!! info "The attaching process"

    The code in this section goes into the process that opened the connection in
    [STREAM](23-stream.en.md). It pairs with the section above.

It authenticates, then receives a notification. The second call is one-way and waits for no
answer, yet a value arrives after it — that is the notification the Actor pushed.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/StreamClient/src/main/kotlin/systems/zlink/tutorial/streamclient/StreamClientProgram.kt:session-actor-client"
```

## 5. What You See When You Run It

With the tutorial Server running as the README's Run section specifies, start StreamClient with the command below: its bind request and received Actor notification appear on the StreamClient process's stdout, while session and Actor handler records appear on the Server process's stdout or in `server.log`.

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
# bound player: p1
# pushed: speedy
```

`pushed` is the point. The client sent a rename and asked for no answer, and what came back is
**a notification the player pushed on its own.** In between, the packet passed the session,
reached the Actor, and the Actor sent back over the same connection.

## 6. After the Connection Drops — the Session Ends and the Actor Stays

The session ends and the Actor stays. A client that returns with the same id is bound to that Actor again.

The callback that tells the Actor about the drop, the rules for a binding moving to another
connection, and what happens while the Actor moves to another node are covered by
[Session and Actor](24-actor-session.en.md).

## 7. Related Documents

- Where the connection is accepted — [STREAM](23-stream.en.md)
- The entity that gets bound — [Actor](22-actor.en.md)
- Disconnect notices and relocation — [Session and Actor](24-actor-session.en.md)
- A running version of this chapter's code — `framework/languages/java/tutorial/kotlin`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
