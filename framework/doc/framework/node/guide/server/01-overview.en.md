---
title: "1. Overview · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/01-overview.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# 1. Overview

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Next: Node.js Quickstart — from Install to a First Request](../../quickstart.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/01-overview.en.md) · [C#/.NET](../../../dotnet/guide/server/01-overview.en.md) · [Java](../../../java/guide/server/01-overview.en.md) · [Kotlin](../../../kotlin/guide/server/01-overview.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can distinguish the problems ZLink Framework solves from its main messaging surfaces.
    The code in this chapter comes from the [language-specific example repositories](https://github.com/zlink-systems/zlink-node-examples).

> This document is the entry point of the Node.js guide. The guide explains the concepts and usage of
> ZLink Framework directly so a TypeScript developer can **read it and start writing code
> right away.** The concepts alone are in
> [Core concepts](03-concepts.en.md).

## 1. One-Line Definition

`ZLink Framework` is a **real-time messaging framework that integrates with the major
framework you already use.** The way Spring MVC sits on Spring as its web layer, ZLink
Framework sits on `NestJS` as a **real-time messaging layer.** It's not a switch to a
separate runtime or a dedicated server — it drops directly into the DI, hosted service,
configuration, and logging model you're already using.

This layer provides inter-server calls, pub/sub, and real-time state units. Inter-server
calls and pub/sub find their target purely by a logical `channel name`, with **no separate
gateway or dedicated load balancer.** The real-time state units are `SPOT` (room · stage ·
zone), an actor (a stateful object representing one connection/user), and `STREAM` (an
external client connection) — if these terms are unfamiliar, see the concept walkthrough in
[03-concepts](03-concepts.en.md) first. A developer writes a **handler, client, and filter**
with the same feel as using HTTP/gRPC, and the framework handles connection, location lookup,
routing, reconnect, and correlation.

> **ZLink is a framework used under the same contract across several languages.** The same
> layer sits identically on Spring (Java/Kotlin) and NestJS (Node) too, and because the call
> contract is a language-neutral wire protocol (ZMP) + codec + logical channel/packet,
> services implemented in different languages call each other over the same channel (e.g., a
> room server in C++, an API server in .NET/Java). This guide is `.NET`-based and treats the
> `.NET` implementation as the reference implementation. The detailed cross-language model is
> covered by [17-alternative §2.1](17-alternative.en.md).

## 2. Make the Adoption Decision in the Scope Chapter

[Where ZLink Applies](17-alternative.en.md) owns the use cases, comparisons with gRPC,
service meshes, Orleans, and Akka, and the boundaries where ZLink should not be adopted.
This overview avoids repeating that technology choice and focuses on the surface and
structure you need after making it.
## 3. Surface and Structure

### 3.1 The Call Unit — MeshName and ChannelName

An inter-server call in ZLink Framework picks its target by **`MeshName` and
`ChannelName`.** In the application, you use it like "send a request over the `orders`
channel in the `services` mesh." Which node handles that channel is decided by the framework,
which checks the membership registered in the location store.

The framework handles what you'd otherwise have written by hand to build one server.

| What you used to build yourself | How the framework handles it |
| --- | --- |
| Opening an endpoint, managing peer connections | Declare a MeshNode and STREAM node, and the hosted service connects them |
| Message serialization/deserialization | Codec registration and the handler contract exchange DTOs directly |
| Request routing/dispatch | Registering a typed handler on a `ChannelName` delivers the message to the right handler |
| Repeating common processing like logging/validation/authorization | An HTTP route uses middleware; a ZLink handler separates this into `ZLinkHandlerFilter` |
| Protecting state under concurrent requests | SPOT's serial execution manages state with no lock |
| Creating services, managing dependencies | NestJS DI creates the handler, client, and filter |
| Managing server addresses, deciding connections | Tracks the currently active endpoint through the location store |
| Configuration, logging, monitoring | Integrated with NestJS configuration/logging/lifecycle |

### 3.2 Technology Choice and Comparisons

Compare the existing approach, alternatives, tradeoffs, and adoption signals in
[Where ZLink Applies](17-alternative.en.md).
### 3.3 Layering and Registration Points

<iframe class="zlink-diagram" src="/common/diagrams/01-layers-en.html" title="Layer structure — ZLink on the host, business logic on top" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-layers-en.html" target="_blank">↗ View larger</a></p>

Put the host framework you already use (ASP.NET Core · Spring Boot · NestJS · C++ host) at
the bottom and register the ZLink Framework into it with `AddZLinkFramework` — the opposite
of bringing in a new engine and moving to a separate ecosystem; all of this runs inside the
framework you already use. On top of that, the only code you write is the **business logic**
(Spot · Actor · handler), and the Framework exposes its own functionality through the
**DI · hosted service · handler · attribute** model.

The point where the application meets this stack is **one registration spot.** This is where
you declare the location store, MeshNode, fanout, and STREAM node. The blocks below splice
together the tutorial's real registration code as-is — the mesh, channel, and fanout names
are the tutorial's own `"game"`/`"profile"`/`"broadcast"`, not `"services"`/`"orders"`/
`"events"`. First, register the location store.

```typescript
--8<-- "framework/languages/node/tutorial/Server/main.ts:location-store"
```

Next, register the mesh and the channel.

```typescript
--8<-- "framework/languages/node/tutorial/Server/main.ts:mesh-register"
--8<-- "framework/languages/node/tutorial/Server/main.ts:channel-register"
```

Then subscribe to the fanout.

```typescript
--8<-- "framework/languages/node/tutorial/Server/main.ts:fanout-subscribe"
```

Finally, register the STREAM node.

```typescript
--8<-- "framework/languages/node/tutorial/Server/main.ts:stream-register"
```

Topologies you used to assemble separately with gRPC+LB, a broker, and a WebSocket server all
collapse down to **one declarative model.** Once the location store is registered,
connections auto-connect and auto-clean-up as servers scale up or down — nothing to edit in
a config file, no LB to reconfigure.
([05](20-channel-messaging.en.md)·[06](21-spot.en.md)·[09](23-stream.en.md)·[10](25-location.en.md))

What you declare, and where, comes down to three spots.

| Surface | Role | Chapter that covers it |
| --- | --- | --- |
| `builder.Services.AddZLinkFramework(...)` | Declare channel/SPOT/STREAM | [Chapter 5](20-channel-messaging.en.md)~[Chapter 9](23-stream.en.md) |
| `options.AddRouteMesh(...)` / `addFanoutChannel(...)` | Declare RouteMesh/fanout | [Chapter 5](20-channel-messaging.en.md) |
| `IZLink*Runtime` status | Status observation and diagnostics | [Chapter 11](26-monitoring.en.md) |

## 4. The Four Integration Axes, Summarized

<iframe class="zlink-diagram" src="/common/diagrams/01-lang-node-en.html" title="ZLink layers — Node/TypeScript" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-lang-node-en.html" target="_blank">↗ View larger</a></p>

| Axis | What the user sees | Guide chapter |
| --- | --- | --- |
| channel messaging | `ZLinkRequestHandler`, `ZLinkSendHandler`, `ZLinkRouteClient`, `ZLinkHandlerFilter` | [05-channel-messaging](20-channel-messaging.en.md) |
| fanout | `addFanoutChannel`, `ZLinkFanoutHandler` | [05-channel-messaging](20-channel-messaging.en.md) |
| SPOT | Typed spot factory, Spot context outbound, timer | [06-spot](21-spot.en.md) |
| actor / session | Actor factory, Entry Spot, `ZLinkBoundSession`, session actor dispatch | [07-actor-spot](22-actor.en.md) · [08-actor-session](24-actor-session.en.md) |
| STREAM | Framework session packet, Stream Connector | [09-stream](23-stream.en.md) |
| Infrastructure | Location-based auto-connect/operational queries, runtime monitoring | [10-location](25-location.en.md), [11-monitoring](26-monitoring.en.md) |
| Operations | Runtime metrics (one registration line), graceful drain, readiness probe | [12-operations](12-operations.en.md) |

## 5. The Overall Topology

An example showing how each feature fits together. Each feature's own chapter zooms into
part of this map.

<iframe class="zlink-diagram" src="/common/diagrams/01-topology-en.html" title="Overall topology" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-topology-en.html" target="_blank">↗ View larger</a></p>

- **API server** — takes the HTTP request and hands it to the domain server. Which way it
  goes depends on the client.
    - **channel client** — calls by name. A request one handler can settle calls a node
      handler over a **ClientServer channel**; a request a unit of state must take goes to an
      Instance Spot over a **RouteMesh channel**.
    - **spot client** — calls a Spot by id.
    - **actor client** — calls an Actor by id.
- **Session server** — takes the client's real-time connection. The STREAM node receives the
  message, the session relay passes it over a **RouteMesh channel**, and the actor in the user
  spot an entry spot assigned handles it.
- **Domain server** — node handlers and spots hold the state and process requests serially.
- **Location store** — manages server address information. The dotted lines are store lookups
  that find an endpoint, not a data path.

**A STREAM node can live in the domain server, but that is not the usual shape.** Connection
count and state throughput grow separately, so it commonly sits in its own server, the way the
HTTP entry does.

## 6. Where to Go Next

Continue with the [Quickstart](../../quickstart.en.md) for installation and the first
call, or [Core Concepts](03-concepts.en.md) for the terms and layers. The
[server guide table of contents](README.en.md) owns the complete learning order and
the role of each chapter.
