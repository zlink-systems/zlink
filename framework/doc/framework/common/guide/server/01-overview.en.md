# 1. Overview

!!! info "What you get from this chapter"

    You can distinguish the problems ZLink Framework solves from its main messaging surfaces.
    The code in this chapter comes from the [language-specific example repositories](https://github.com/zlink-systems/zlink-<language>-examples).

=== "C#/.NET"

    > This document is the entry point of the `.NET` guide. The guide explains the concepts and usage of
    > ZLink Framework directly so an `ASP.NET Core` developer can **read it and start writing
    > code right away.** The concepts alone are in
    > [Core concepts](03-concepts.en.md).

=== "C++"

    > This document is the entry point of the C++ guide. The guide explains the concepts and usage of
    > ZLink Framework directly so a C++ developer can **read it and start writing code right
    > away.** The concepts alone are in
    > [Core concepts](03-concepts.en.md).

=== "Java"

    > This document is the entry point of the Java guide. The guide explains the concepts and usage of
    > ZLink Framework directly so a Java developer can **read it and start writing code right
    > away.** The concepts alone are in
    > [Core concepts](03-concepts.en.md).

=== "Kotlin"

    > This document is the entry point of the Kotlin guide. The guide explains the concepts and usage of
    > ZLink Framework directly so a Kotlin developer can **read it and start writing code
    > right away.** The concepts alone are in
    > [Core concepts](03-concepts.en.md).

=== "Node/TypeScript"

    > This document is the entry point of the Node.js guide. The guide explains the concepts and usage of
    > ZLink Framework directly so a TypeScript developer can **read it and start writing code
    > right away.** The concepts alone are in
    > [Core concepts](03-concepts.en.md).

## 1. One-Line Definition

=== "C#/.NET"

    `ZLink Framework` is a **real-time messaging framework that integrates with the major
    framework you already use.** The way Spring MVC sits on Spring as its web layer, ZLink
    Framework sits on `ASP.NET Core` as a **real-time messaging layer.** It's not a switch to
    a separate runtime or a dedicated server — it drops directly into the DI, hosted service,
    configuration, and logging model you're already using.

=== "C++"

    `ZLink Framework` is a **real-time messaging framework.** In other languages it sits as a
    layer on `ASP.NET Core` or Spring Boot, but **C++ has no such standard application
    framework.** So the C++ framework also provides DI, configuration, and HTTP hosting, and
    composes the process itself. Instead of dropping into a model you already have, it gives
    you that model too.

=== "Java"

    `ZLink Framework` is a **real-time messaging framework that integrates with the major
    framework you already use.** The way Spring MVC sits on Spring as its web layer, ZLink
    Framework sits on `Spring Boot` as a **real-time messaging layer.** It's not a switch to
    a separate runtime or a dedicated server — it drops directly into the DI, hosted service,
    configuration, and logging model you're already using.

=== "Kotlin"

    `ZLink Framework` is a **real-time messaging framework that integrates with the major
    framework you already use.** The way Spring MVC sits on Spring as its web layer, ZLink
    Framework sits on `Spring Boot` as a **real-time messaging layer.** It's not a switch to
    a separate runtime or a dedicated server — it drops directly into the DI, hosted service,
    configuration, and logging model you're already using.

=== "Node/TypeScript"

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
> room server in C++, an API server in .NET/Java). Each language guide describes the same public
> contract, and its examples use that language’s actual API.

## 2. Adoption Decision

See [Where ZLink Applies](17-alternative.en.md) for use cases and technology selection criteria.
This chapter describes ZLink’s main surfaces and structure.
## 3. Surface and Structure

### 3.1 The Call Unit — MeshName and ChannelName

An inter-server call in ZLink Framework picks its target by **`MeshName` and
`ChannelName`.** In the application, you use it like "send a request over the `orders`
channel in the `services` mesh." Which node handles that channel is decided by the framework,
which checks the membership registered in the location store.

The framework handles what you'd otherwise have written by hand to build one server.

=== "C#/.NET"

    | What you used to build yourself | How the framework handles it |
    | --- | --- |
    | Opening an endpoint, managing peer connections | Declare a MeshNode and STREAM node, and the hosted service connects them |
    | Message serialization/deserialization | Codec registration and the handler contract exchange DTOs directly |
    | Request routing/dispatch | Registering a typed handler on a `ChannelName` delivers the message to the right handler |
    | Repeating common processing like logging/validation/authorization | An HTTP route uses middleware; a ZLink handler separates this into `IZLinkHandlerFilter` |
    | Protecting state under concurrent requests | SPOT's serial execution manages state with no lock |
    | Creating services, managing dependencies | ASP.NET Core DI creates the handler, client, and filter |
    | Managing server addresses, deciding connections | Tracks the currently active endpoint through the location store |
    | Configuration, logging, monitoring | Integrated with ASP.NET Core configuration/logging/hosted service |

=== "C++"

    | What you used to build yourself | How the framework handles it |
    | --- | --- |
    | Opening an endpoint, managing peer connections | Declare a MeshNode and STREAM node, and the hosted service connects them |
    | Message serialization/deserialization | Codec registration and the handler contract exchange DTOs directly |
    | Request routing/dispatch | Registering a typed handler on a `ChannelName` delivers the message to the right handler |
    | Repeating common processing like logging/validation/authorization | An HTTP route uses middleware; a ZLink handler separates this into a handler filter |
    | Protecting state under concurrent requests | SPOT's serial execution manages state with no lock |
    | Creating services, managing dependencies | The framework's DI container creates the handler, client, and filter |
    | Managing server addresses, deciding connections | Tracks the currently active endpoint through the location store |
    | Configuration, logging, monitoring | The framework's built-in config/logging/hosted service |

=== "Java"

    | What you used to build yourself | How the framework handles it |
    | --- | --- |
    | Opening an endpoint, managing peer connections | Declare a MeshNode and STREAM node, and the hosted service connects them |
    | Message serialization/deserialization | Codec registration and the handler contract exchange DTOs directly |
    | Request routing/dispatch | Registering a typed handler on a `ChannelName` delivers the message to the right handler |
    | Repeating common processing like logging/validation/authorization | An HTTP route uses middleware; a ZLink handler separates this into `ZLinkHandlerFilter` |
    | Protecting state under concurrent requests | SPOT's serial execution manages state with no lock |
    | Creating services, managing dependencies | Spring DI creates the handler, client, and filter |
    | Managing server addresses, deciding connections | Tracks the currently active endpoint through the location store |
    | Configuration, logging, monitoring | Integrated with Spring configuration/logging/lifecycle |

=== "Kotlin"

    | What you used to build yourself | How the framework handles it |
    | --- | --- |
    | Opening an endpoint, managing peer connections | Declare a MeshNode and STREAM node, and the hosted service connects them |
    | Message serialization/deserialization | Codec registration and the handler contract exchange DTOs directly |
    | Request routing/dispatch | Registering a typed handler on a `ChannelName` delivers the message to the right handler |
    | Repeating common processing like logging/validation/authorization | An HTTP route uses middleware; a ZLink handler separates this into `ZLinkHandlerFilter` |
    | Protecting state under concurrent requests | SPOT's serial execution manages state with no lock |
    | Creating services, managing dependencies | Spring DI creates the handler, client, and filter |
    | Managing server addresses, deciding connections | Tracks the currently active endpoint through the location store |
    | Configuration, logging, monitoring | Integrated with Spring configuration/logging/lifecycle |

=== "Node/TypeScript"

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

### 3.2 Layering and Registration Points

<iframe class="zlink-diagram" src="/common/diagrams/01-layers-en.html" title="Layer structure — ZLink on the host, business logic on top" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-layers-en.html" target="_blank">↗ View larger</a></p>

Register ZLink Framework with the existing host framework (ASP.NET Core · Spring Boot ·
NestJS · C++ host) to run without a separate runtime. The application places its
**business logic** (Spot · Actor · handler) above it. The Framework provides its features
through **DI · hosted service · handler · attribute**.

The point where the application meets this stack is **one registration spot.** This is where
you declare the location store, MeshNode, fanout, and STREAM node. The blocks below splice
together the tutorial's real registration code as-is — the mesh, channel, and fanout names
are the tutorial's own `"game"`/`"profile"`/`"broadcast"`, not `"services"`/`"orders"`/
`"events"`. First, register the location store.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:location-store"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:location-store"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:location-store"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:location-store"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:location-store"
    ```

Next, register the mesh and the channel.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:mesh-register"
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:channel-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:channel-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:mesh-register"
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:channel-register"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:mesh-register"
    --8<-- "framework/languages/node/tutorial/Server/main.ts:channel-register"
    ```

Then subscribe to the fanout.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:fanout-subscribe"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:fanout-subscribe"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:fanout-subscribe"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:fanout-subscribe"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Server/main.ts:fanout-subscribe"
    ```

Finally, register the STREAM node.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:stream-register"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Server/main.cpp:stream-register"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:stream-register"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Server/src/main/kotlin/systems/zlink/tutorial/server/ServerApplication.kt:stream-register"
    ```

=== "Node/TypeScript"

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
| `options.AddRouteMesh(...)` / `AddFanoutChannel(...)` | Declare RouteMesh/fanout | [Chapter 5](20-channel-messaging.en.md) |
| `IZLink*Runtime` status | Status observation and diagnostics | [Chapter 11](26-monitoring.en.md) |


## 4. The Four Integration Axes, Summarized

=== "C#/.NET"

    <iframe class="zlink-diagram" src="/common/diagrams/01-lang-dotnet-en.html" title="ZLink layers — .NET" style="width:100%;border:0"></iframe>
    <p><a href="/common/diagrams/01-lang-dotnet-en.html" target="_blank">↗ View larger</a></p>

    | Axis | What the user sees | Guide chapter |
    | --- | --- | --- |
    | channel messaging | `IZLinkRequestHandler`, `IZLinkSendHandler`, `IZLinkRouteClient`, `IZLinkHandlerFilter` | [05-channel-messaging](20-channel-messaging.en.md) |
    | fanout | `AddFanoutChannel`, `IZLinkFanoutHandler` | [05-channel-messaging](20-channel-messaging.en.md) |
    | SPOT | Typed spot factory, Spot context outbound, timer | [06-spot](21-spot.en.md) |
    | actor / session | Actor factory, Entry Spot, `IZLinkBoundSession`, session actor dispatch | [07-actor-spot](22-actor.en.md) · [08-actor-session](24-actor-session.en.md) |
    | STREAM | Framework session packet, Stream Connector | [09-stream](23-stream.en.md) |
    | Infrastructure | Location-based auto-connect/operational queries, runtime monitoring | [10-location](25-location.en.md), [11-monitoring](26-monitoring.en.md) |
    | Operations | Runtime metrics (one `AddMeter` line), graceful drain, readiness probe | [12-operations](12-operations.en.md) |

=== "C++"

    <iframe class="zlink-diagram" src="/common/diagrams/01-lang-cpp-en.html" title="ZLink layers — C++" style="width:100%;border:0"></iframe>
    <p><a href="/common/diagrams/01-lang-cpp-en.html" target="_blank">↗ View larger</a></p>

    | Axis | What the user sees | Guide chapter |
    | --- | --- | --- |
    | channel messaging | Request handler, send handler, `request_client_t`, handler filter | [05-channel-messaging](20-channel-messaging.en.md) |
    | fanout | `AddFanoutChannel`, fanout handler | [05-channel-messaging](20-channel-messaging.en.md) |
    | SPOT | Typed spot factory, Spot context outbound, timer | [06-spot](21-spot.en.md) |
    | actor / session | Actor factory, Entry Spot, `bound_session_t`, session actor dispatch | [07-actor-spot](22-actor.en.md) · [08-actor-session](24-actor-session.en.md) |
    | STREAM | Framework session packet, Stream Connector | [09-stream](23-stream.en.md) |
    | Infrastructure | Location-based auto-connect/operational queries, runtime monitoring | [10-location](25-location.en.md), [11-monitoring](26-monitoring.en.md) |
    | Operations | Runtime metrics (one registration line), graceful drain, readiness probe | [12-operations](12-operations.en.md) |

=== "Java"

    <iframe class="zlink-diagram" src="/common/diagrams/01-lang-java-en.html" title="ZLink layers — Java" style="width:100%;border:0"></iframe>
    <p><a href="/common/diagrams/01-lang-java-en.html" target="_blank">↗ View larger</a></p>

    | Axis | What the user sees | Guide chapter |
    | --- | --- | --- |
    | channel messaging | `ZLinkRequestHandler`, `ZLinkSendHandler`, `ZLinkRouteClient`, `ZLinkHandlerFilter` | [05-channel-messaging](20-channel-messaging.en.md) |
    | fanout | `AddFanoutChannel`, `ZLinkFanoutHandler` | [05-channel-messaging](20-channel-messaging.en.md) |
    | SPOT | Typed spot factory, Spot context outbound, timer | [06-spot](21-spot.en.md) |
    | actor / session | Actor factory, Entry Spot, `ZLinkBoundSession`, session actor dispatch | [07-actor-spot](22-actor.en.md) · [08-actor-session](24-actor-session.en.md) |
    | STREAM | Framework session packet, Stream Connector | [09-stream](23-stream.en.md) |
    | Infrastructure | Location-based auto-connect/operational queries, runtime monitoring | [10-location](25-location.en.md), [11-monitoring](26-monitoring.en.md) |
    | Operations | Runtime metrics (one registration line), graceful drain, readiness probe | [12-operations](12-operations.en.md) |

=== "Kotlin"

    <iframe class="zlink-diagram" src="/common/diagrams/01-lang-kotlin-en.html" title="ZLink layers — Kotlin" style="width:100%;border:0"></iframe>
    <p><a href="/common/diagrams/01-lang-kotlin-en.html" target="_blank">↗ View larger</a></p>

    | Axis | What the user sees | Guide chapter |
    | --- | --- | --- |
    | channel messaging | `ZLinkRequestHandler`, `ZLinkSendHandler`, `ZLinkRouteClient`, `ZLinkHandlerFilter` | [05-channel-messaging](20-channel-messaging.en.md) |
    | fanout | `AddFanoutChannel`, `ZLinkFanoutHandler` | [05-channel-messaging](20-channel-messaging.en.md) |
    | SPOT | Typed spot factory, Spot context outbound, timer | [06-spot](21-spot.en.md) |
    | actor / session | Actor factory, Entry Spot, `ZLinkBoundSession`, session actor dispatch | [07-actor-spot](22-actor.en.md) · [08-actor-session](24-actor-session.en.md) |
    | STREAM | Framework session packet, Stream Connector | [09-stream](23-stream.en.md) |
    | Infrastructure | Location-based auto-connect/operational queries, runtime monitoring | [10-location](25-location.en.md), [11-monitoring](26-monitoring.en.md) |
    | Operations | Runtime metrics (one registration line), graceful drain, readiness probe | [12-operations](12-operations.en.md) |

=== "Node/TypeScript"

    <iframe class="zlink-diagram" src="/common/diagrams/01-lang-node-en.html" title="ZLink layers — Node/TypeScript" style="width:100%;border:0"></iframe>
    <p><a href="/common/diagrams/01-lang-node-en.html" target="_blank">↗ View larger</a></p>

    | Axis | What the user sees | Guide chapter |
    | --- | --- | --- |
    | channel messaging | `ZLinkRequestHandler`, `ZLinkSendHandler`, `ZLinkRouteClient`, `ZLinkHandlerFilter` | [05-channel-messaging](20-channel-messaging.en.md) |
    | fanout | `AddFanoutChannel`, `ZLinkFanoutHandler` | [05-channel-messaging](20-channel-messaging.en.md) |
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
