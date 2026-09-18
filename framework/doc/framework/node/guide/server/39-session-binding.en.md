---
title: "How Session Binding Works · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/39-session-binding.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# How Session Binding Works

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: How STREAM Works](38-stream-boundary.en.md) | [Next: 17. Where ZLink Applies — Internal Service Communication and Real-Time State Servers](17-alternative.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/39-session-binding.en.md) · [C#/.NET](../../../dotnet/guide/server/39-session-binding.en.md) · [Java](../../../java/guide/server/39-session-binding.en.md) · [Kotlin](../../../kotlin/guide/server/39-session-binding.en.md) · **Node/TypeScript**
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

Choosing which Actor to relay to on a session with several bindings is the application's part. The
application picks the actor id by its own protocol and passes it to the lookup call — **the
framework does not pick an arbitrary Actor.**

The binding call treats a duplicate bind as an error. A flow that may already be bound, such as a
resent authentication, uses the find-or-bind call instead.

## 2. Binding and Spot Membership — Independent of Each Other

Binding is separate from the Actor's Spot membership. Even when the Actor moves to another Spot or
another node, the actor id and the incarnation value are kept and the framework refreshes the
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
look the same reference up again and bind.

**A failed notification for one Actor does not stop the rest.** The framework fixes the list of
bindings held when the connection dropped and notifies each Actor; one of them failing, or a
callback overrunning its deadline, does not stop the remaining notifications or the session cleanup.

**An automatic notification overlapping an explicit call still runs the callback once.** The
framework merges the two notifications for the same binding, so a drop right after an explicit call
does not run the Spot's disconnect callback twice.

## 4. When a Bind Fails or Becomes Void

| Situation | Result |
| --- | --- |
| The Actor does not exist, or is not in a state to receive | The bind ends with a typed error |
| The reference's incarnation value differs | A stale reference is not bound to another incarnation |
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
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
