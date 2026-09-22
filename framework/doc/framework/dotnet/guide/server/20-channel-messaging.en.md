---
title: "Channel Messaging · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/20-channel-messaging.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Channel Messaging

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 3. Core Concepts](03-concepts.en.md) | [Next: Spot](21-spot.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/20-channel-messaging.en.md) · **C#/.NET** · [Java](../../../java/guide/server/20-channel-messaging.en.md) · [Kotlin](../../../kotlin/guide/server/20-channel-messaging.en.md) · [Node/TypeScript](../../../node/guide/server/20-channel-messaging.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

The code in this chapter comes from the [tutorial README](https://github.com/zlink-systems/zlink-dotnet-examples/blob/main/tutorial/README.md). Download the [examples repository](https://github.com/zlink-systems/zlink-dotnet-examples/blob/main/tutorial/README.md) and follow its README's Download, Build and Run sections to reproduce the results below.

!!! info "What you get from this chapter"

    You can register and call the three arrangements servers use to call each
    other. The code in every section runs as it stands in
    `framework/languages/dotnet/tutorial`.

When a server calls another server, it does not name the other side's address. The caller names
only a **name**, and the Framework delivers it to the **node** that serves that name. A node is
one server process running the Framework.

This chapter covers the case where that name is a **channel name**. It goes as far as
registering and calling; the constraints of each pattern and the boundaries of selection are
covered by [How Channels Work](30-channel-patterns.en.md).

## 1. Request and Response

### 1.1 Message Contracts

A contract is an ordinary record. It needs no separate registration or attribute declaration. A
one-way message has no matching response type.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Shared/Contracts.cs:channel-contracts"
```

### 1.2 A Handler That Returns a Response

The handler's return value becomes the response as it stands.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Channel/GetPlayerProfileHandler.cs:channel-request-handler"
```

### 1.3 A Handler with No Response

It receives a message that arrived by `send`. There is no return value, so the caller cannot
learn the result of the processing.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Channel/RecordLoginHandler.cs:channel-send-handler"
```

### 1.4 A Call Is Sent by Its Terminator

Every call in this chapter has **a leading part that assembles it and one last piece that sends
it**. The leading part that picks the target name and the message only assembles the call;
nothing has been sent yet. The runtime accepts the call at the moment you invoke that last piece.
**Leave it out and nothing happens at all** — in the calling code in the tabs below, it is what
comes at the end of the line.

That last piece comes in two kinds, and **"finished" means something different for each.**

A `request` is finished **when the other side's response has arrived**. A failure means the other
side could not process it, or no response came. The response type you receive is decided here
rather than by the message you send — the same request payload is used in places that receive
different response types.

A `send` is finished **when my own runtime accepted the transmission**. Success means it was
sent, not that it was processed, and a processing failure on the other side is not visible. When
you need the result of the processing, use `request`.

## 2. The Three Channel Messaging Arrangements

Channel messaging comes in three arrangements. All three call by name; what differs is **who
receives that name**. The three sections below take them in turn.

**[RouteMesh](#3-routemesh)** — the arrangement where servers call each other. The participating
servers are connected into one web, and either side can call the other. When several servers
serve the same name, the Framework sends to one of them. A system where calls travel between
servers in both directions belongs here.

**[ClientServer](#4-clientserver-channel)** — the arrangement of a web server. The server opens a
port and the caller connects to that address. Run several servers doing the same work and the
caller picks one of them to send to. Calls go one way; the server does not call the caller first.

**[Fanout](#5-fanout-channel)** — the publish/subscribe arrangement. Send once and every
subscribed server receives it. Use it when everyone has to receive the same news, as with a
maintenance notice. The sender does not know who is subscribed.

All three arrangements use the message contracts and handlers from
[Request and Response](#1-request-and-response) as they are. What changes is how you register.

!!! note "When one fixed target has to receive it, a channel is the wrong tool"

    When **the recipient is already decided** — a message that changes the state of `player-123`,
    say — do not send it to a channel. A channel picks one of the servers serving that name.
    There is a separate path for such messages —
    [Calling a Spot or an Actor](#37-calling-a-spot-or-an-actor).

A per-item comparison of the three arrangements, and their constraints, are in
[How Channels Work](30-channel-patterns.en.md).

## 3. RouteMesh

RouteMesh provides four calls, depending on **what you name**.

- **A channel name** — `RequestToChannel` · `SendToChannel`. One of the nodes serving that name
  receives it — [calling by channel name](#32-the-receiving-side--the-node-serving-the-channel).
- **A node** — `RequestToNode` · `SendToNode`. The node you named receives it —
  [calling a node directly](#36-calling-a-node-directly).
- **A Spot id** — `RequestToSpot` · `SendToSpot`. The Spot with that id receives it —
  [Calling a Spot or an Actor](#37-calling-a-spot-or-an-actor).
- **An Actor id** — `RequestToActor` · `SendToActor`. The Actor with that id receives it —
  [Calling a Spot or an Actor](#37-calling-a-spot-or-an-actor).

All four calls use the same mesh connection. **Only the first needs a channel registration.**

### 3.1 How It Works

<iframe class="zlink-diagram" src="/common/diagrams/20-routemesh-bidirectional-en.html" title="RouteMesh — both sides call each other" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-routemesh-bidirectional-en.html" target="_blank">↗ View larger</a></p>

Nodes that joined the same mesh can call each other. When A serves the `profile` channel and B
serves the `match` channel, A calls B and B calls A. There is one connection per mesh, and the
two calls share it.

One channel is still one direction, from the caller to the serving node. Two-way is made **by
two nodes each registering as the server of a different channel.**

When several nodes serve the same channel, the caller does not name one of them. The Framework
picks one among the ready serving nodes. Add nodes and the candidate set grows.

### 3.2 The Receiving Side — the Node Serving the Channel

!!! note "The receiving process"

    The code in this section goes into the process that **handles** the request.

Both nodes have to name the mesh identically. And only a handler exposed with `Server()` can be
called by another node — being in the same assembly is not enough; without registration it is not
a call target.

The `listen` host is the address where the current process binds its socket; `0.0.0.0` accepts
connections on every local interface. `AdvertiseHost` is the address peers actually dial and the Location Store
publishes in the MeshNode descriptor. A wildcard bind is not an address remote processes can dial,
so the single-machine tutorial uses `127.0.0.1`. In a multi-host, container, NAT, or Kubernetes
deployment, set a reachable IP address or DNS name for that node, using a Pod IP or per-Pod DNS in
Kubernetes. One Service address cannot represent several Pods when each node endpoint must be
distinguished. When `AdvertiseHost` is omitted,
[Network Listener Identity §2.1](../../../common/spec/server/02-channel-transport/04-network-listener-identity.en.md#21-defaults)
uses a non-wildcard bind host, or the same-family loopback (`127.0.0.1` for `0.0.0.0`, `::1` for
`::`) for a wildcard bind; a wildcard is not allowed as an advertised host. Each language's mesh
registration block shows that language's actual option surface.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"

--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
```

### 3.3 The Calling Side — a Node Registering the Same Channel as a Client

!!! info "The calling process"

    The code in this section goes into the other process, the one that **sends** the request. It
    pairs with the section above.

The calling node opens an endpoint of its own too. Both sides have to open one to connect as
peers. There is one connection per mesh, and several channels share that one connection,
distinguished by name.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-client-register"
```

### 3.4 The Call

A call names only the channel name. It does not name which node will receive it.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-request-call"

--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-send-call"
```

### 3.5 What You See When You Run It

With the tutorial Server and Client running as the README's Run section specifies, send these `curl` requests to the Client's HTTP surface: responses appear on `curl` stdout, and the login record appears on the Server process's stdout or in `server.log`.

```bash
curl http://127.0.0.1:5080/players/p1/profile
# {"playerId":"p1","nickname":"rookie","level":1}

curl -X POST http://127.0.0.1:5080/players/p1/logins
# 202. The receiving node logs: login recorded: p1
```

### 3.6 Calling a Node Directly

So far nothing has named which node receives. The second way names one MeshNode by its
`RoutingId`. It creates no channel and picks no candidate — the node you named answers, or the
call fails.

!!! warning "Use this for operational commands only"

    Use it only when **the node itself** is the target, as with a health check or an operational
    command. Do not use it to choose where an Actor or Spot is created, or to pin a business
    message to a particular server. Business messages use a logical name — a channel name, a Spot
    id, an Actor id — because the Framework picks the current server and the application keeps no
    node RID.

<iframe class="zlink-diagram" src="/common/diagrams/20-node-direct-en.html" title="A business call names a name, an ops call names a node RID" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-node-direct-en.html" target="_blank">↗ View larger</a></p>

#### The Receiving Side — Registered Straight onto the Mesh

!!! note "The receiving process"

    Goes into the process that answers with its state.

It does not go through `Channel(...)`. The handler implements the channel request handler
interface — for the exact name, read your language's tab below.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Ops/NodeStatusHandler.cs:node-direct-handler"
```

The receiving node **has to fix its id.** Without that, the Framework attaches a generated id and
the caller cannot name it.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"

--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:node-direct-register"
```

#### The Calling Side — Being Connected to the Mesh Is Enough

!!! info "The calling process"

    Goes into the process that sends the operational command.

**There is nothing extra to register.** Being connected to that node as a peer is enough to call
it by id. The code below is the same registration used for the channel call.

You can write the expected id alongside the connection. That fences the connection to that one
node, and a peer answering with a different id is refused at the handshake.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:channel-client-register"
```

The call takes the mesh name and the target id together.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:node-direct-call"
```

#### What You See When You Run It

With the tutorial Server and Client running as the README's Run section specifies, send these `curl` requests to the Client's HTTP surface: HTTP responses appear on `curl` stdout, and the node-direct handler record appears on the Server process's stdout or in `server.log`.

```bash
curl http://127.0.0.1:5080/ops/nodes/game-server-1/status
# {"meshName":"game","channelName":"(none)",
#  "calledBy":"game-2a0af167-...","uptime":"14s","processId":39892}

curl -i http://127.0.0.1:5080/ops/nodes/no-such-node/status
# 404. No candidate is picked, so it fails as it stands.
```

`channelName` is empty. That means no channel took part; had it been a channel handler, the
channel name would appear here.

### 3.7 Calling a Spot or an Actor

A **Spot** is a stateful object found by id. Like one chat room or one matchmaking queue, it
remembers something and processes the work addressed to it in a single line. An **Actor** is also
an execution unit found by id, holding per-entity state such as one player or one session.

Both can run on any node of the mesh, and may move to another node while running. **RouteMesh
finds which node that target is on right now.** The caller gives only the id.

<iframe class="zlink-diagram" src="/common/diagrams/20-spot-actor-routing-en.html" title="A Spot or Actor call goes where the id is" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-spot-actor-routing-en.html" target="_blank">↗ View larger</a></p>

A call that does not wait for an answer gives only the id and the message.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:spot-send-call"
```

When you need an answer, send a request at the same place. The target may be moving, so give a
timeout alongside it.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:spot-request-call"
```

This is where it differs from a channel call. A channel **picks one** of the serving nodes; a Spot
or Actor call goes **where the target with that id is**. So a message that a fixed target has to
receive uses this path, not a channel.

Registration, lifecycle, state management, and moving between locations are covered by
[Spot](21-spot.en.md) and [Actor](22-actor.en.md).

## 4. ClientServer Channel

Writing a handler is the same as for RouteMesh. Only the registration differs.

### 4.1 How It Works

<iframe class="zlink-diagram" src="/common/diagrams/20-clientserver-oneway-en.html" title="ClientServer — calls run one way" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-clientserver-oneway-en.html" target="_blank">↗ View larger</a></p>

Only the server publishes an address, and the client starts the connection too. **A server never
starts a new call toward a client.** All that goes back to a client is the response to a request
the client sent first.

When a call in the reverse direction is needed, stand the other node up as a server too and make a
separate channel. Unlike RouteMesh, one registration does not make it two-way.

### 4.2 The Receiving Side — the Server That Opens a Port

!!! note "The receiving process"

    Goes into the process that handles the request.

Separately from the mesh, it opens its own port and separately announces the address to reach it
from outside, because the caller connects to that address directly.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:clientserver-register"
```

### 4.3 The Calling Side — the Client That Connects to That Address

!!! info "The calling process"

    Goes into the process that sends the request.

The calling side connects the server endpoint directly. The calling code itself is the same as for
RouteMesh.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:clientserver-client-register"

--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:clientserver-call"
```

### 4.4 What You See When You Run It

With the ClientServer Server and Client running as the tutorial README's Run section specifies, send these `curl` requests to the Client's HTTP surface: responses appear on `curl` stdout, and handler records appear on the Server process's stdout or in `server.log`.

```bash
curl -X POST http://127.0.0.1:5080/players/p1/tickets
# "ticket-p1"
```

## 5. Fanout Channel

### 5.1 How It Works

<iframe class="zlink-diagram" src="/common/diagrams/20-fanout-topic-en.html" title="Fanout — every subscribed node receives it" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/20-fanout-topic-en.html" target="_blank">↗ View larger</a></p>

The publishing side does not name the nodes that will receive. One `publish` is delivered to every
node subscribed to that **topic**. Leave the topic out and the Framework uses the event's packet
name as the topic.

The publishing side keeps no subscriber list. Adding or removing a subscribing node does not
change the publishing code.

!!! note "Fanout has no response"

    With several recipients there is no way to settle on a single response to return. A Fanout
    client provides `publish` only, and has no call corresponding to `request`.

### 5.2 The Receiving Side — a Subscribing Node

!!! note "The receiving process"

    Goes into the process that receives the event.

The receiving handler uses a different interface from the two above. There is nobody to return to,
so there is no return value either.

The subscribing side does not name the publisher address. With a Location Store registered it is
resolved automatically, and naming a manual connection alongside it is refused at startup.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Channel/MaintenanceNoticeSubscriber.cs:fanout-handler"

--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:fanout-subscribe"
```

### 5.3 The Calling Side — the Publishing Node

!!! info "The calling process"

    Goes into the process that sends the event.

The publishing side registers as a publisher and then calls publish. It does not name the
recipients.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:fanout-publish-register"

--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:fanout-call"
```

### 5.4 What You See When You Run It

With the Server and Client containing the fanout publisher and subscriber running as the tutorial README's Run section specifies, send these `curl` requests to the Client's HTTP surface: HTTP responses appear on `curl` stdout, and received records appear on the subscriber process's stdout or in `server.log`.

```bash
curl -X POST http://127.0.0.1:5080/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# 202. The subscribing node logs: maintenance notice: scheduled maintenance
```

## 6. Related Documents

Calling by naming something other than a channel name is covered by its own chapter.

- Calling a Spot by Spot id — [Spot](21-spot.en.md)
- Calling an Actor by Actor id — [Actor](22-actor.en.md)
- Connecting an external client — [STREAM](23-stream.en.md)

The next two chapters go deeper.

- Wiring, target selection, connection and discovery, startup validation —
  [How Channels Work](30-channel-patterns.en.md)
- Packet names, filters, codecs — [Handlers and Message Processing](31-handler-dispatch.en.md)
- A running version of this chapter's code — `framework/languages/dotnet/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
