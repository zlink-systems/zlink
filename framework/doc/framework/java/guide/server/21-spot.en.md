---
title: "Spot · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/21-spot.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Spot

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Channel Messaging](20-channel-messaging.en.md) | [Next: Actor](22-actor.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/21-spot.en.md) · [C#/.NET](../../../dotnet/guide/server/21-spot.en.md) · **Java** · [Kotlin](../../../kotlin/guide/server/21-spot.en.md) · [Node/TypeScript](../../../node/guide/server/21-spot.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can create a stateful object addressed by id, send messages to it, and
    receive answers. The code comes from the [tutorial README in the examples repository](https://github.com/zlink-systems/zlink-java-examples/blob/main/tutorial/README.md); follow its Download, Build, and Run sections to reproduce the results below.

In [Channel Messaging](20-channel-messaging.en.md) a call was received by one of the nodes
serving the name. That path does not work when the recipient is already decided. **A Spot is a
stateful object found by id**, and it processes the work addressed to it in a single line. This
chapter goes as far as creating one Spot and calling it; the differences between the kinds and
the full lifecycle are covered by [Spot](21-spot.en.md).

## 1. The Role of a Spot

Spot creates units that **remember something and process the work addressed to them in order** —
one chat room, one matchmaking queue. A channel cannot express that: a channel call goes to one of
the nodes serving that name, so two messages sent to the same room can arrive at different
nodes.

A Spot has an id. Messages sent to the same id always arrive at the same Spot, and inside that
Spot they run **one at a time**. A Spot's fields therefore need no lock.

<iframe class="zlink-diagram" src="/common/diagrams/21-spot-placement-en.html" title="A Spot is created on one of the nodes that registered it" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/21-spot-placement-en.html" target="_blank">↗ View larger</a></p>

Neither the creator nor the caller picks a node. That much matches a channel; what differs is
that **the target is settled to exactly one**.

## 2. The Location Store — a Prerequisite for Registering a Spot

A Spot is called by id, not by host. That requires **somewhere to record which node it is on
right now**. That is the Location Store, and it is required once you register even one Spot.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:location-store"
```

**Register a Relocation Store alongside it.** Registering a Spot factory is itself the
condition, so it is required even with relocation turned off as it is in this chapter.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:relocation-store"
```

A node that only calls needs the Location Store alone —
[the calling side](#4-the-calling-side--the-node-that-calls-a-spot).

## 3. The Receiving Side — the Node That Hosts the Spot

!!! note "The receiving process"

    The code in this section goes into the process that **runs** the Spot.

### 3.1 Writing the Spot

A Spot holds its state in fields and initializes itself from the payload it receives when it is created.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/spots/GameRoom.java:spot-class"
```

To refuse at creation, refuse in the create callback. The create call then fails and no Spot
remains. Omit that callback and every create request is accepted.

### 3.2 Writing the Handlers

A handler receives the target Spot as its first argument. Unlike a channel handler, it **works
on that Spot's state directly.**

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/spots/GameRoom.java:spot-handlers"
```

A handler with no return value receives a message that arrived by `send`; a handler that returns
a value makes that value the response. Within one room the two handlers run **one at a time**.

### 3.3 Registration

The mesh node picks the Object Server role once, and the Spot factory is registered on it.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:object-server"

--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:spot-register"
```

The name given at registration is the **stable type**. The calling side names it when opening a
room, and the nodes that registered the same name become the candidates. Exactly one relocation
policy is required; this chapter turns it off. To preserve state through an adapter, choose
`preserveStateWith` at this registration — [Relocation's adapter](37-relocation.en.md#2-the-applications-part--the-adapter)
shows the code that captures and restores that state.

## 4. The Calling Side — the Node That Calls a Spot

!!! info "The calling process"

    The code in this section goes into the other process, the one that creates the room and
    **calls** it. It pairs with the section above.

The calling node does not run the Spot, so it registers no factory. It picks the client role on
that mesh instead, and registers the Location Store it reads locations from under the same
prefix.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:location-store-client"

--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:spot-client-register"
```

### 4.1 Creating

Create by naming the stable type. The id that comes back is the address for every later call.

First, the tutorial `Server` defines the `game` route mesh. A mesh name names the set of nodes
where a Spot may be placed, and `inMesh` selects one such set. [The receiving side of Channel
Messaging](20-channel-messaging.en.md#32-the-receiving-side--the-node-serving-the-channel) shows
how to register a route mesh, and [Location Runtime §7](../../../common/spec/server/05-location-relocation/01-location-runtime.en.md#7-creating-an-actor-or-user-spot) defines the selection rule.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:mesh-register"
```

The call that creates the Spot in that mesh follows.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:spot-create-call"
```

### 4.2 Calling

Give only the id received at creation. The Framework finds which node it is on right now.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:spot-send-call"
```

When you need an answer, send a request. The target may be moving, so give a timeout alongside it.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:spot-request-call"
```

## 5. What You See When You Run It

With the tutorial Server and Client running as the README's Run section specifies, send these `curl` requests to the Client's HTTP surface: each HTTP response appears on `curl` stdout, and handler records appear on the Server process's stdout or in `server.log`.

```bash
curl -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "8213420c-03ff-47a6-8410-ddfb3df26660"

curl -X POST http://127.0.0.1:5080/rooms/8213420c-03ff-47a6-8410-ddfb3df26660/chat \
  -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
# 202

curl http://127.0.0.1:5080/rooms/8213420c-03ff-47a6-8410-ddfb3df26660
# {"title":"lobby","chat":["p1: hello"]}
```

The Framework makes the id. The second call does not wait for a response; the third receives the
answer the room produced. The room held its state between the two calls.

## 6. The Other Kinds of Spot

What this chapter created is **the Spot an application creates explicitly**, and most rooms,
stages, and zones are that one. There is also the kind the Framework creates when the Object
Server starts, and the kind created when the first message arrives. They differ in when they are
created and in which lifecycle callbacks they receive.

The differences between the kinds and the full lifecycle are covered by [Spot](21-spot.en.md).

## 7. Related Documents

- The other unit addressed by id — [Actor](22-actor.en.md)
- The path that calls by name — [Channel Messaging](20-channel-messaging.en.md)
- What the Location Store records — [How Channels Work](30-channel-patterns.en.md#6-connection-and-discovery)
- A running version of this chapter's code — [the tutorial README in the examples repository](https://github.com/zlink-systems/zlink-java-examples/blob/main/tutorial/README.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
