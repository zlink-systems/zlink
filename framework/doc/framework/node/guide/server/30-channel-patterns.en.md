---
title: "How Channels Work · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/30-channel-patterns.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# How Channels Work

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Relocation](37-relocation.en.md) | [Next: Handlers and Message Processing](31-handler-dispatch.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/30-channel-patterns.en.md) · [C#/.NET](../../../dotnet/guide/server/30-channel-patterns.en.md) · [Java](../../../java/guide/server/30-channel-patterns.en.md) · [Kotlin](../../../kotlin/guide/server/30-channel-patterns.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

The code in this chapter comes from the [tutorial README](https://github.com/zlink-systems/zlink-node-examples/blob/main/tutorial/README.md) and the [`TicTacToe`](https://github.com/zlink-systems/zlink-node-examples/blob/main/samples/TicTacToe/README.md) and [`ZoneWorld`](https://github.com/zlink-systems/zlink-node-examples/blob/main/samples/ZoneWorld/README.md) sample READMEs. Download the [examples repository](https://github.com/zlink-systems/zlink-node-examples/blob/main/tutorial/README.md) and follow each README's Download, Build and Run sections to reproduce the results below.

!!! info "What you get from this chapter"

    You learn which connection each of the three arrangements opens, who it picks
    as a target, and when it refuses. This chapter's code comes from the
    repository's samples and tutorials.

[Channel Messaging](20-channel-messaging.en.md) covered how to register and how to call. This
chapter covers **why it behaves that way and how far it goes**: the differences between the
patterns, the target selection rules, connection and discovery, startup validation, and what the
caller sees when a call fails.

## 1. The Patterns Compared

| | RouteMesh | ClientServer | Fanout |
| --- | --- | --- | --- |
| Call direction | Both sides call each other | Client to server only | Publisher to subscriber only |
| Who decides the receiver | The Framework | The set of servers the caller connected to | Nothing decides |
| Nodes one call reaches | One serving node | One selected server | Every subscriber |
| Calls available | `request` · `send` | `request` · `send` | `publish` |
| Response | Only `request` receives one | Only `request` receives one | None |
| Connection | Many channels share one mesh peer connection | The client connects to the endpoint the server published | A subscriber SUB socket per publisher PUB endpoint |
| When the sending node is itself a Server | **Not a candidate** | A candidate like any other server | Not applicable |
| When there are no candidates | Fails immediately with no target | Waits briefly, then fails | Succeeds with no receiving node |
| Loss | None | None | **Drops a slow subscriber's share** (with NoDrop, ends with an error) |

The calling code is the same for RouteMesh and ClientServer. `sendToChannel` and
`requestToChannel` pick a process-local send path from the ChannelName alone, and whether that
path is RouteMesh or ClientServer is decided by the registration. **Switching between the two
patterns is a change to the registration only.**

## 2. Physical Wiring — What Actually Gets Connected

The three arrangements are not just different names: **they open different sockets.** A process
that uses all three ends up with three separate listeners.

<iframe class="zlink-diagram" src="/common/diagrams/30-wiring-en.html" title="The three patterns open different sockets" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/30-wiring-en.html" target="_blank">↗ View larger</a></p>

| | Listener it opens | Unit of connection | When you add a channel |
| --- | --- | --- | --- |
| RouteMesh | One ROUTER per MeshNode | A peer connection between nodes | **No socket is added** |
| ClientServer | One per Server channel | A connection for that channel | One more listener per channel |
| Fanout | One PUB per publisher | publisher ↔ subscriber SUB | One more per publisher |

### 2.1 RouteMesh — Many Channels Share One Connection

A MeshNode has one routing id and one ROUTER endpoint for peers to connect to. However many
ChannelNames you register on it, **they all use that same ROUTER connection.** Adding a
ChannelName adds neither a socket nor a connection between nodes.

So among nodes already in the mesh, the cost of a new channel is the name registration alone.
Conversely, a process that has not joined the mesh cannot call that channel at all.

A manual connection registers the endpoint to connect to on the MeshNode. You can give the
endpoint alone, or give the routing id of the node expected to be there alongside it.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

### 2.2 ClientServer — Each Channel Opens Its Own Listener

A ClientServer Server **opens its own port and announces its address**, independent of the mesh.
Two channels mean two listeners, and the two channels share neither their connection targets nor
their lifetimes. If the same process also joins a mesh, the ROUTER listener exists separately
alongside them.

A client connects to that address directly, so **it can call without joining the mesh.** That is
why this arrangement is used to call a service built by another team or a separate deployment
unit.

### 2.3 Fanout — A Separate PUB/SUB Socket Pair

A Fanout publisher opens a PUB listener, and a subscriber uses a dedicated SUB socket per
publisher endpoint. It shares no socket with a MeshNode or a Spot. Delivery is therefore
independent of the mesh arrangement, and it does not share a target set with
[Logical Multicast](#51-logical-multicast--events-between-spots-on-the-mesh) either.

RouteMesh, ClientServer, the fanout publisher, and a STREAM server are all separate listeners,
and they share the process's default network values. When one listener needs a different bind or
advertise address, specify the override on that listener.

## 3. Call Direction

### 3.1 RouteMesh — Role Registration Decides the Direction

RouteMesh itself has no direction. Direction is decided by the role a node registered per
ChannelName.

- Only a node that registered `Server()` receives calls on that channel.
- A node that registered only `Client()` can start a call but does not receive one.
- **The `Server()` role includes the ability to send.** Do not register `Client()` again for the
  same ChannelName.

You can register several channels on one MeshNode, and each channel may carry a different role.

```typescript
--8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/zone-node-module.ts:doc-multi-channel-register"
```

So when A is the Server of `api` and B is the Server of `billing`, both A→B and B→A hold. The
two calls use the same peer connection but are different channels. **One channel does not become
two-way; two channels are registered in opposite directions.**

The list of roles cannot change after startup. The one thing that can change while running is
the weight of a Server membership —
[Stopping Only New Requests While Running](#43-stopping-only-new-requests-while-running).

### 3.2 ClientServer — The Direction Is Fixed

ClientServer is a **one-way service boundary**. The following are fixed together.

| What is fixed | Content |
| --- | --- |
| Business call direction | A server never starts a new call toward a client |
| Connection start direction | With manual and automatic discovery alike, only the client starts a connection to the server |

What a client receives from a server is **only the reply matching a request it started**. It does
not receive a message the server sent first, without a client request.

When a call in the reverse direction is needed, the node on the other side has to become a server
that publishes its own endpoint, and that is a separate channel with a separate ChannelName.

One process may register a Client and a Server once each for the same ChannelName. The
`client_and_server` value in a monitoring snapshot expresses that both registrations are present;
it is not a role of its own.

## 4. How a ToChannel Call Picks a Target Node

These are **the rules that decide where `sendToChannel` and `requestToChannel` go**. They apply
only to the RouteMesh and ClientServer arrangements. A call that names a node directly, and a
Spot or Actor call, already have their target, so there is no selection; `publish` does not pick
a target at all.

### 4.1 Target Selection — a Server in the Same Process Is Excluded Only on a RouteMesh

One process can be **both the caller and the side that handles that channel**. Whether the Server
in its own process counts as a candidate differs between the two arrangements.

| | RouteMesh | ClientServer |
| --- | --- | --- |
| When a Server registration for that channel is present in the same process | **Not a candidate** | A candidate like any other server |
| When no Server exists anywhere but that process | **Fails with no target** | The server in its own process is selected |
| When there is not a single candidate yet | Fails immediately | **Waits briefly**, then fails |

**ClientServer allows one process to register a Client and a Server once each for the same
ChannelName.** That process then both calls and handles. Its own server enters the candidate set
on the same terms as a remote server — readiness, weight, whether it has begun shutting down —
and being local neither gives it priority nor excludes the remote ones.

Even when selected, **the handler is not called directly.** The call goes through the regular send
path, so codec, timeout, cancellation, and correlation apply exactly as they do for a remote one.
There is no separate path that calls a local handler straight away.

**RouteMesh excludes its own process for a structural reason.** A channel registration creates no
new socket and uses **the peer connection that already exists**, and a MeshNode does not form a
peer connection with itself. So when that channel's Server exists only in its own process, there
is no path to send on. To have it handled in the same process, use ClientServer.

The waiting side has a reason too. In RouteMesh, having no candidate means no peer has published
that name, and waiting will not produce one. In ClientServer, the configuration may already be
present in the same process and only its preparation unfinished. So it waits for **the shorter of
the call's timeout and five seconds**. That wait does not hurry the preparation along; it only
waits for preparation already in progress to finish.

### 4.2 Distribution Among Candidates

Some targets drop out of the candidate set first. This is the same in both arrangements.

| Target excluded | Reason |
| --- | --- |
| A target that is not ready | Initialization and the discovery record are not finished |
| A target whose weight is `0` | Membership is kept, but it drops out of new selections |
| A target that has begun a safe shutdown | It is finishing the requests it has and going down |

**When removing those three leaves no target at all, both `request` and one-way `send` end as
`NotFound`.** No target produces the same result for the two calls.

Distribution among the remaining targets is **decided by weight.** Weight ranges over `0..10000`
and defaults to `100`.

**Leave it alone and every weight is `100`, which makes it an even round-robin.** However many
servers you run, new requests are distributed in turn. Most arrangements stop here.

Give different values and traffic is distributed in that ratio. Two candidates at `100` and `300`
come to roughly `1:3` over the long run — **which does not mean the order of individual calls is
guaranteed.** Use it when mixing machines of different specifications, or when one machine should
receive less traffic.

The initial value is set at registration. Give it with `setWeight(...)` where the channel role is
registered. Leave it out and it is `100`. Changing it while running is covered by
[Stopping Only New Requests While Running](#43-stopping-only-new-requests-while-running).

<iframe class="zlink-diagram" src="/common/diagrams/05-node-select-en.html" title="Round-robin distribution · adding a node reflected automatically" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/05-node-select-en.html" target="_blank">↗ View larger</a></p>

### 4.3 Stopping Only New Requests While Running

Ahead of maintenance or a rolling restart, you sometimes want a node **to stop accepting new
requests** without taking it down. Change its weight to `0`. **This is the one value you can
change while running.** Inject the RouteMesh runtime options and name the ChannelName.

```typescript
--8<-- "framework/languages/node/tutorial/Server/main.ts:weight-runtime"
```

- `Weight = 0` does **not close** the serving socket. Requests already received are handled and
  answered to the end, and other nodes drop this node only as a target for new requests. Its
  registration in the Location Store also stays.
- **When this is the only node serving that channel, new calls end as `Unavailable`.** The
  candidate set is empty; the path is not broken —
  [Distribution Among Candidates](#42-distribution-among-candidates).
- `Weight = 100` is the normal return.
- Propagation is not immediate. What is guaranteed is **that the signal was sent**; whether
  another node actually dropped it from its candidates is confirmed from that node's state
  ([Monitoring](26-monitoring.en.md)).
- Operations commonly call these two actions `drain` and `restore`. That is a name an
  application's admin API gave to `Weight = 0` and `= 100`; **the Framework provides no API by
  those names.**

### 4.4 Growing One ChannelName Across Several Nodes

To raise throughput, run several providers serving the same MeshName and ChannelName.

**Nothing changes on the handling side.** Start one more process that registered the same
ChannelName with `Server()`. The registration code is the same regardless of the node count.

**Nothing changes on the calling side either, if you use a Location Store.** The Store holds the
new provider's registration, so the candidate set grows on its own.

In an arrangement that writes addresses directly, register every provider endpoint. Below is the
code that connects to two handling nodes.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

In that case, every time you add a provider you have to change the caller's configuration and
restart it. That is why an arrangement whose node count changes uses a Location Store.

**An unregistered ChannelName is not looked up elsewhere.** Even with another MeshNode or a
ClientServer client in the same process, nothing is sent there instead. Conversely, registering
the same ChannelName on both RouteMesh and ClientServer in one process is **refused at startup** —
one name has to point at exactly one send path.

When a particular entity — an order id, a user id — must always be handled by the same execution
unit, use a Spot or an actor rather than a channel ([Spot](21-spot.en.md)).

### 4.5 Channel Calls Started by a Spot Handler

A Spot handler or timer can start a channel send/request. **That ChannelName doesn't have to
exist on the MeshNode that owns the Spot** -- as long as one send route with that name is
registered anywhere in the same process, it's usable. It can be a route on a different
RouteMesh, or a ClientServer client's route.

**Route resolution stops at the process boundary.** The route isn't resolved through a relay
on another process or MeshNode -- it ends with `NotFound`. That's why, when deciding which node
to place a Spot on, you also check **whether a send route for the channels that Spot calls
is registered in the same process.**

## 5. The Forms of Pub/Sub

Publishing has **no target selection.** Whoever subscribed receives it. That publish/subscribe
also comes in two different forms. The names are similar enough to mix up, but **their sockets,
target scope, and loss rules all differ.**

| | Logical Multicast | Classic fanout |
| --- | --- | --- |
| Socket | Uses the already-connected mesh socket as it is | Opens an independent PUB/SUB socket pair |
| Who receives | The **Spots** subscribed to the same topic on that channel | **Every connected subscriber** |
| Relation to the mesh arrangement | Confined to that mesh | Unrelated |
| Loss | Not applicable | **Allowed** (not configurable today) |
| Filter | Does not run | Runs |
| Where it is registered | When the Spot starts | The fanout channel builder |

### 5.1 Logical Multicast — Events Between Spots on the Mesh

A [Spot](21-spot.en.md) is a stateful object found by id, and it processes the work addressed to
it in a single line. Exchanging events between such Spots over a RouteMesh channel is called
Logical Multicast. There is no separate socket, and the receiving side is confined to the Spots
subscribed to the same topic on that channel.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts:doc-multicast-publish"
```

The publish call takes a topic. Some languages take a ChannelName alongside it; others use the
mesh the Spot sits on as it is — read your language's tab above.

A subscription opens by registering the handler that will receive that topic when the Spot starts.
**Where the topic is written in that registration differs by language** — it is passed as an
argument to the registration call, or marked on the handler type.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts:doc-multicast-subscribe"
```

To publish from outside a Spot, inject the spot publisher client and send it the same way.

### 5.2 Classic Fanout — Delivery That Allows Loss

A fanout channel opens an independent PUB/SUB socket pair of its own. Independent of Spots and
MeshNodes, one publisher delivers to every connected subscriber.

**The loss rule is different.** When a subscriber receives slowly and the publisher's send queue
reaches its limit (`sendHighWaterMark`), **that subscriber's share is discarded and the publish
ends as a success.** The remaining subscribers are unaffected, and the publisher does not stall on
account of one slow subscriber.

**Turn on NoDrop when a slow subscriber's share must not be discarded.** The slow subscriber's
backpressure then waits the publish, and the publish ends with an error if the queue does not clear
before its deadline.

```typescript
builder.addFanoutChannel('events').enablePublisher('tcp://*:7400').setNoDrop(true);
```

NoDrop is available only on a channel with the publisher capability. Logical Multicast provides no
storage, retransmission, or acknowledgements either.

### 5.3 Topic — Both Forms Select Receivers by It

A topic selects receivers in both forms. Logical Multicast selects Spots; classic fanout selects
subscriber sockets.

| | Logical Multicast | Classic fanout |
| --- | --- | --- |
| Subscription registration | ChannelName + **topic** | ChannelName + **topic** |
| Does the topic filter delivery | **It does.** Only Spots subscribed to the same topic receive it | **It does.** Only subscribers subscribed to the same topic receive it |
| After receiving | The subscription handler of that Spot processes it | A handler is found by packet name; with none, it is discarded |

A classic fanout subscriber registers each topic it receives with `subscribe(topic)`. Only events
published to a matching topic travel to that subscriber.

```typescript
builder.addFanoutChannel('events').enableSubscriber().subscribe('order.created');
```

The publish context received by a handler also holds the arriving topic, so one handler can
register several topics and branch on it.

### 5.4 How to Set the Topic When Publishing

- `Publish(channelName, message)` — no topic is given. The event's **packet name** becomes the
  topic.
- `Publish(channelName, topic, message)` — the topic is set directly.
- Passing a reserved topic is refused with an `ArgumentException`. Those are values the Framework
  uses to check connection state.

Neither form keeps a per-subscriber acknowledgement or replay state. An event published while a
subscriber was not connected is not delivered later.

## 6. Connection and Discovery

| | Who publishes the address | Who starts the connection | Discovery |
| --- | --- | --- | --- |
| RouteMesh | Both nodes open an endpoint of their own | One side is chosen so the two nodes do not start at once | The MeshNode descriptor |
| ClientServer | The server | The client | A manual `connect`, or the server descriptor in the Location Store |
| Fanout | The publisher | The subscriber | The publisher descriptor in the Location Store |

### 6.1 Location Store — Where Who-Is-Where Is Written Down

The **Location Store** that keeps appearing in the table above is a store kept outside the
Framework. It is a component of the system, used by running processes to find one another. The
tutorials and samples use Redis.

<iframe class="zlink-diagram" src="/common/diagrams/30-location-store-en.html" title="Location Store — where who-is-where is written down" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/30-location-store-en.html" target="_blank">↗ View larger</a></p>

What it does is simple. **Each process writes its own address and the names it serves into the
Store at startup**, and refreshes that record while it lives. The record disappears when it stops.
The caller reads the Store to find where to connect.

So adding servers or restarting one at a different address **changes nothing in the caller's
configuration.** That is where this parts ways with writing addresses directly via `connect(...)`.

The Store holds more than addresses.

| What is written | Where it is used |
| --- | --- |
| A MeshNode's address and the ChannelNames it serves | RouteMesh automatic connection |
| A ClientServer Server's address | Finding what a client should connect to |
| A Fanout publisher's address | Finding what a subscriber should connect to |
| The node a Spot or Actor is on right now | Resolving the location when calling by id |
| The creation authority that keeps one Spot from being created in two places | Spot creation |

**A Store is required in order to use Spots and Actors.** Those features decide "which node is it
on right now" by reading the Store. If you use only channel messaging and write addresses
directly, it works without one.

The registration code is in [Spot](21-spot.en.md#2-the-location-store--a-prerequisite-for-registering-a-spot),
and the operational queries are in
[Operations and Lifecycle](12-operations.en.md#5-location-readiness-and-operational-queries).

**There are two stores.** The Location Store handles the atomic changes to small location records;
the Relocation Store holds what remains after a move — the record of an Instance Spot's first
activation and the terminal record of a request that completes after a move. The moving state, the
queue and the timers themselves do not pass through a store; they go straight from the departing
node to the arriving node over the mesh connection.

| Store | When it is required |
| --- | --- |
| Location Store | **Required** on a MeshNode whose object role is client or server. Without it, startup fails as a configuration error before a socket is opened |
| Relocation Store | **Required whenever even one** factory carries a relocation policy or an Instance Spot factory exists. It may be omitted only when relocation is off everywhere and there is no Instance Spot factory |

**Each is registered exactly once.** There is no surface that bundles the two into one registration
call, and missing a required one or registering more than one is a configuration error before a
socket opens. The framework does not build a substitute store inside the process when one is absent
— **it fails rather than quietly running as a single node.**

The two stores may share one Redis deployment. Keep their key prefixes distinct. The framework does
not rely on transactions spanning the stores, so the physical Redis may be separated as well. After
registering a store, the application neither calls the provider's operations directly nor disposes
of it — the framework manages the store's lifetime and call order.

### 6.2 The Owner Lease — the Mark That Ownership Is Alive

An owner refreshes a mark that it is alive at intervals, and once that mark expires another node may
take the place over. The refresh interval, the expiry, the ceiling on one refresh, and the margin
that cuts work off ahead of expiry — **these four values are tied together.** Breaking the relation
is a startup error. Look at all four when changing one. The value names and defaults are owned by
the per-language options chapter.

A host whose lease refresh has stopped **accepts no new work** from the moment it passes the
computed time — state-changing messages, timer starts, committing factory and restore results,
changing relocation state and reserving capacity are all blocked. Finishing and cleaning up work
already in the queue continues. It is the device that stops a new owner and an old owner writing at
once.

### 6.3 While the Store Is Down

There is a grace period for a store outage. It is the time the last fully read node list is kept —
**not time added to ownership.**

| During the grace | Result |
| --- | --- |
| Connections already established | State decisions continue |
| New outbound connections | None are made. Even after the grace ends, none are made until the whole node list is read again as of one point in time |
| The owner lease and relocation deadlines | **They are not extended** |

Requests already received are kept; what stops is the computation that adds to and removes from the
target list. When the store recovers, the list is reconciled against the latest registrations.

### 6.4 Manual and Automatic

A manual connection is configured in the MeshNode's peer list.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

The endpoint argument is a startup setting. It is not a handle that controls a running socket
after the host starts. The one value that can change while running is the weight in
[Stopping Only New Requests While Running](#43-stopping-only-new-requests-while-running).

In automatic connection mode, the Location Store owns the peer list. When a server restarts at a
new endpoint, the store's descriptor row is updated and the client connection follows, so no
separate action is needed. **A manual connection applies only after you change the configuration
and restart the application.**

A fanout subscriber **cannot specify automatic discovery and a manual endpoint together.**
Registering both is refused at startup.

### 6.5 Finding It in the Store Is Not Yet Sending to It

After obtaining an endpoint from the registration, a client **re-confirms identity and lifecycle
generation on the actual connection** before it uses that target. A manual connection goes through
the same confirmation. So a call can end with no target even though the row is in the store — at
that point, look at **whether the connection was established**, not at the store.

**Restarting a server changes the lifecycle generation**, which says which run of the node this is.
Even with the same endpoint, a connection from the previous lifecycle generation is not used as a
new target; the client prepares the new value and then removes the previous connection. Lifecycle
generation values are not ordered by numerical size.

A reply that arrives late **becomes the result if the original request is still waiting** — even
when it came from a previous generation. Conversely, if that request is gone through timeout,
cancellation, or a client restart, it is discarded and **never used as the result of another
request started later.**

## 7. What It Means for a Call to Be Finished

| Call | When it is finished |
| --- | --- |
| `request` | The reply the target handler returned has arrived |
| `send` | The source-local queue accepted the message |
| `publish` | Source-local publish admission finished |

Finishing a `send` or a `publish` **guarantees no delivery.** `publish` returns neither a
subscriber count nor a receipt confirmation. When you need the result of the processing, choose a
pattern where `request` can be used.

**Sending with a packet that has no handler** differs by path.

| Call | Result |
| --- | --- |
| `request` | Fails with an error reply. The caller receives it as an exception |
| `send` | Dropped silently |

Dropped means the caller gets no reply, not that nothing is observable. The logger and telemetry
providers you configured receive the dispatch failure as a `no_handler`, `reply_error`, or `drop`
structured record ([Monitoring](26-monitoring.en.md)).

## 8. Related Documents

- How to register and call — [Channel Messaging](20-channel-messaging.en.md)
- Collecting shared processing in one place — [Filters](31-handler-dispatch.en.md#2-filters--collecting-shared-processing-in-one-place)
- Serialization codecs — [Codecs](31-handler-dispatch.en.md#3-codecs--turning-a-payload-into-bytes)
- A running version of this chapter's code — `framework/languages/dotnet/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
