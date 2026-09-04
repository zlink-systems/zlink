---
title: "3. Core Concepts · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/03-concepts.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 2. Getting Started](02-getting-started.en.md) | [Next: 4. Backpressure — When Arrival Outpaces Processing](04-backpressure.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/03-concepts.en.md) · **C++** · [Java](../../../java/guide/server/03-concepts.en.md) · [Kotlin](../../../kotlin/guide/server/03-concepts.en.md) · [Node/TypeScript](../../../node/guide/server/03-concepts.en.md)
<!-- language-switch:end -->

# 3. Core Concepts

> **The documents that own this chapter's contract** — [Framework Overview](../../../common/spec/server/00-foundation/03-overview.en.md)
> and the [Interaction Model](../../../common/spec/server/00-foundation/04-interaction-model.en.md) own the
> formal meaning of the concepts, and the
> [per-language handler interface contracts](../../../common/spec/server/languages/README.en.md)
> own the formal definition of the interfaces. This document lays out what that meaning
> looks like in code.

The ZLink framework provides **channel · spot · actor · stream · location** as its core
concepts. Every other chapter is a variation on these. We'll go through them in order below,
and cover [relocation](#5-relocation--moving-to-another-node) — an actor or spot moving to
another node partway through — along the way. Later dedicated chapters cover how to implement
and operate each concept in practice.

## 1. channel — a connection between servers

**MeshNode** is the basic unit of a connection between servers. A single MeshNode can take
on two independent roles.

- **Object role** — the place where spots and actors are placed. Covered separately in
  [spot](#2-spot--a-unit-that-owns-state-and-processes-it-in-order) and
  [actor](#3-actor--a-state-object-identified-by-id).
- **Channel role** — the place where request/send/publish are exchanged. Covered in this
  document.

`ChannelName` is a logical name that groups together the nodes in that mesh sharing the same
function — instead of an address (`host:port`), you pick a call target by a name like
`"orders"`. The caller passes only the `ChannelName` to the route client — `MeshName` is
fixed at registration and never appears in the call arguments.

The caller doesn't need to know which node is currently handling that request. Pass only a
logical name (`ChannelName`, a spot id, or an actor id) — not an address or node number —
and the framework finds the target and delivers the request wherever that name currently
lives. This property
— **the caller doesn't need to know where the target is** — is called location
transparency. Channels, spots, and actors all work this way. That's why the calling code stays
the same whether you scale servers out or in.

When a message is sent by `ChannelName`, the framework selects one of the nodes currently
able to receive the request and delivers it there — this selection is called
**select-one**.

<iframe class="zlink-diagram" src="/common/diagrams/03-channel-select-en.html" title="channel — calling by name (select-one)" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-channel-select-en.html" target="_blank">↗ View larger</a></p>

If three nodes own the same `orders` channel, one of them is selected per call. The caller
doesn't know — and doesn't need to know — which node was selected.

Here's what it looks like to configure both roles on one MeshNode.

```cpp
auto mesh = options.add_route_mesh ("services");  // One MeshNode joins the mesh "services".
mesh.listen ("tcp://0.0.0.0:7101");               // Its own endpoint for other nodes to connect to.

// C++ sets the Object role with one enum instead of a separate builder.
mesh.set_object_role (object_role_t::server);     // This node places spots/actors.
mesh.channel_name ("orders").server ();           // Channel role — this node handles "orders" requests.
mesh.channel_name ("billing").client ();          // A call-only channel is client.
```

Automatic connection management, which avoids hard-coding peer addresses and tracks servers
as they scale up or down, is covered by [10-location](10-location.en.md).

> **Note:** `MeshName` and `ChannelName` are different names. You can register several
> `ChannelName`s on one mesh, and the same `ChannelName` can be used across different
> meshes.

Several registration types use the name "channel"; they differ in whether they share a
socket.

| Kind | Socket |
| --- | --- |
| route mesh channel | Shares the already-open MeshNode socket |
| ClientServer channel | Opens its own socket, separate from the MeshNode |
| fanout channel | Opens its own dedicated PUB/SUB socket |

pub/sub also splits two ways. **Logical Multicast**, exchanged between Spots over a route
mesh channel, uses the mesh socket as-is, and a **fanout channel** delivers to every
connected subscriber over its own socket. The structural comparison of the three and how to
use them is covered by
[05-channel-messaging §1](05-channel-messaging.en.md#1-channel-kinds).

## 2. spot — a unit that owns state and processes it in order

Many domains — a game room, a guild, or an auction item — have **state that several requests
may access at the same time.**
Building this yourself means both finding the process that currently holds the state and
routing requests there, while also preventing concurrent access by incoming requests. Keep
the state in process memory and you have to manage that routing yourself; keep it in a DB or
Redis and you have to read, write, and lock on every request.

A spot lets the framework handle both concerns. It keeps the target as **a single in-memory
object** and processes incoming requests **serially, one at a time.**
Since two requests can never touch the same state at the same time, no lock is needed.

Unlike a channel, a spot is addressed by id. Send to the `"orders"` channel and any node
capable of doing that work handles it. Send a request to a spot id like `"room-42"`,
though, and the node where that spot is located receives the message and hands it to that
spot to process. The framework finds that node through the same location transparency
[seen earlier](#1-channel--a-connection-between-servers).

<iframe class="zlink-diagram" src="/common/diagrams/03-spot-queue-en.html" title="spot — owns state, processes in order" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-spot-queue-en.html" target="_blank">↗ View larger</a></p>

A spot is registered on the MeshNode's **Object role**. It's a separate surface from the same
MeshNode's Channel role.

Spots fall into three kinds — **Entry Spot · User Spot · Instance Spot** — based on when
they're created, and **execution mode** determines which work runs concurrently. The
differences between the three kinds, choosing an execution mode, and
registration/lifecycle/timer/outbound are covered by [06-spot](06-spot.en.md).

## 3. actor — a state object identified by ID

An actor is a **stateful object identified by an ID.** A message that arrives with the same
ID is always handled by the same instance. An actor always belongs to some spot, and how it
binds to an external client connection continues in the
[next section](#4-stream--external-client-connections).

<iframe class="zlink-diagram" src="/common/diagrams/03-actor-route-en.html" title="actor — identified by id" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-actor-route-en.html" target="_blank">↗ View larger</a></p>

See [07-actor-spot](07-actor-spot.en.md) for details.

## 4. stream — external client connections

A stream is a **connection-oriented, bidirectional channel to an external client** such as
a mobile app or game client. Unlike a server-to-server
[channel](#1-channel--a-connection-between-servers), the server manages connection lifecycle
and heartbeats, and one connection maps to a server-side **session** object. Reconnecting
after a disconnect is the client connector's job.

When you **bind** a session to an [actor](#3-actor--a-state-object-identified-by-id), the
session stops handling messages that arrive over that connection itself and relays them to
the bound actor instead. The reverse direction works the same way — a push from the actor
goes out to the client through the session bound to that actor.

<iframe class="zlink-diagram" src="/common/diagrams/03-stream-en.html" title="stream — external client connection" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-stream-en.html" target="_blank">↗ View larger</a></p>

So **the node that accepts the connection and the node that runs domain logic can be split.**
Even if the session lives on a gateway node and the actor lives on a different node, the
framework keeps the relay path alive. Even if the actor moves via
[relocation](#5-relocation--moving-to-another-node), the same session remains connected to
the actor at its new location.

See [09-stream](09-stream.en.md) for details; how to bind a session to an actor is covered by
[08-actor-session](08-actor-session.en.md).

## 5. relocation — moving to another node

Relocation is when an actor or spot leaves its current owner node and keeps running on
another node. It starts from two distinct triggers.

An actor belongs to a spot, and a spot belongs to a node. Relocation preserves this
relationship exactly as it is — only the node it runs on changes.

**When an actor joins a spot on another node.** If an actor requests to join a User Spot
that lives on a different node, the moment the join is accepted the actor moves to that node
carrying its state and pending work along with it. This is a move the application triggers by
request.

<iframe class="zlink-diagram" src="/common/diagrams/03-relocation-en.html" title="relocation — an actor joins a spot on another node" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-relocation-en.html" target="_blank">↗ View larger</a></p>

The only thing the join call specifies is the **spot id `room-42`** — there's no argument
that names a target node. The framework looks up which node currently holds that spot in the
location store and moves actor P to that node. So the names "node A" and "node B" never
appear anywhere in the application code. Once the move finishes, actor P belongs to node B's
`room-42` spot and follows the same execution rules as Q and R.

**When an operator relocates a host for maintenance or deployment without downtime.** The
operator moves the spots and actors on one host to another host. The framework handles this
without individual join requests from the application, and once it's done, the original host
can be shut down.

<iframe class="zlink-diagram" src="/common/diagrams/03-host-relocate-en.html" title="host relocate — moving spot and actors as a whole" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/03-host-relocate-en.html" target="_blank">↗ View larger</a></p>

A server holding state can't simply be taken down, so maintenance or deployment usually
means dropping the connection and making clients wait. Host Relocate preserves the spot's
and actor's state while moving them to another node, freeing up node A. The caller still
sends requests to the same id, `room-42`, so it never needs to know the move happened. The
net effect is that **a stateful service can be replaced with zero downtime, the way a
stateless service would be.**

Both paths follow **the same relocation policy.** What happens to application state during
the move (not moved, recreated, or restored as-is) is fixed at spot/actor factory
registration and doesn't change while running.

Policy types and selection criteria are covered by
[07-actor-spot §1](07-actor-spot.en.md); making an actor join call and receiving the
completion result are covered by [07-actor-spot §5](07-actor-spot.en.md); and Host Relocate
as zero-downtime maintenance/deployment, along with how a relocation unit is scoped, is
covered by [12-operations §2](12-operations.en.md).

## 6. location — address resolution

Application code uses only logical names like a channel name, and the actual peer address
(`host:port`) is resolved by a **location store** shared across the whole deployment. Each
server registers its own location as a descriptor in the store on startup, and the calling
side looks up the target in the store by logical name and connects. When the server topology
changes, connections update accordingly.

Usage is covered by [10-location](10-location.en.md); the contract is defined by the
[common spec](../../../common/spec/server/05-location-relocation/01-location-runtime.en.md).

Manual connections — specifying an endpoint directly at registration without a store — are
also supported, for development/testing and small fixed deployments
([05-channel-messaging §6](05-channel-messaging.en.md)). You can't use both approaches
together on the same MeshNode.

> **See it in a sample — [TicTacToe](../../../common/sample/tictactoe/README.en.md).** This
> is the smallest example where all of these concepts show up in one sample. They all meet
> in one place: the Play server's registration code.
>
> | Concept | In TicTacToe |
> | --- | --- |
> | channel | The Play server looks up credentials over an independent `tictactoe.api` ClientServer Channel |
> | spot | One match is one `TicTacToeGame` spot — both players' moves are processed serially inside it |
> | actor | A player is an actor, and even after a reconnect the same actor picks up the ongoing match |
> | stream | The client connects directly to the Play STREAM endpoint from the API response to make moves and receive pushes |
> | location | The Redis location store automatically picks which Play node will create a new `TicTacToeGame` spot — the API code has no specific Play node address |
>
> You've seen what problem each concept solves above; this sample shows **what it looks like
> when they're put together.**

## 7. Where to Start for What You're Trying to Do

Once the concepts are clear, the next question is, "So which surface do I use?" There are
**eight starting points**, all obtained from DI or the current handler context.
**The application never picks a transport socket or endpoint directly.**

| What you're trying to do | Starting surface | What you specify |
| --- | --- | --- |
| Send directly to a node, or by channel name | route client | Node-direct is MeshName + target RID; channel is ChannelName |
| Send to a Spot | spot client | Global Spot ID |
| Send to an Actor | actor client | Global Actor ID |
| Create or find a User Spot | spot manager | Spot type, and a global Spot ID if needed |
| Create or find an Actor | actor manager | Global Actor ID and Actor type |
| Publish a Logical Multicast | spot publisher client | ChannelName and topic |
| Publish classic pub/sub | fanout client | fanout ChannelName, and topic if needed |
| Send to or reply to a STREAM client | session client | The current session |

The exact type names are language-specific and are defined in the `13. Interface Catalog`
chapter.

**Completion has two consistent forms.** A send-family call finishes with
no return value once **the send slot accepts it**, and a request-family call finishes with
one of **reply · timeout · route error**. This holds no matter which surface you use
([04-backpressure §3](04-backpressure.en.md#3-backpressure-visible-in-the-api)).

## 8. What the Framework Owns and What It Doesn't

**Everything below is handled internally by the framework.** None of it appears in
application code.

| Owned |
| --- |
| Transport address selection and peer reconnect |
| Multipart framing and packet codec |
| Reply correlation |
| Backpressure queue |

**These, by contrast, are outside this framework's contract scope.** They're handled by the
external edge.

| Not owned |
| --- |
| External client authentication and quota |
| WAF |
| Public API versioning |
| Billing |

Server-to-server communication and a real-time state server are this framework's territory.
Policy for an edge exposed directly to the internet is owned by whatever sits in front of it.

## 9. Related Documents

- Full usage of request/send/pub-sub, writing a handler, and the `async` execution model:
  [05-channel-messaging](05-channel-messaging.en.md)
- Spot kinds, execution model, handler lifetime, and DI scope: [06-spot](06-spot.en.md)
- Host lifecycle and operations: [12-operations](12-operations.en.md)
- Registration points and layering: the [01. Overview](01-overview.en.md)
- The full interface/attribute/context set:
  [per-language handler interface contracts](../../../common/spec/server/languages/README.en.md)
- Runnable sample code: [14-samples](14-samples.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=Math.max(d.body?d.body.scrollHeight:0,d.documentElement?d.documentElement.scrollHeight:0);if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
