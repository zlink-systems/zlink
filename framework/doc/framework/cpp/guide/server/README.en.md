---
title: "Guide Home · C++"
---

# ZLink Framework C++ — User Guide

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/server/README.en.md) · [Java](../../../java/guide/server/README.en.md) · [Kotlin](../../../kotlin/guide/server/README.en.md) · [Node/TypeScript](../../../node/guide/server/README.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

A C++ application framework for building **server systems where real-time messaging
matters** out of several cooperating processes.

```cpp
#include <zlink/framework.hpp>

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.http ()
          .listen ("http://0.0.0.0:8080")
          .map_post<open_conversation_http_handler_t> ("/conversations");
    });
    return app.run (argc, argv);
}
```

Register one handler class and the framework handles message decoding, routing, and
encoding.

---

## Systems It Is Built For

It's designed for systems where several server processes split responsibilities and
cooperate, and where a state change must reach the client in real time.

| Domain | Core scenario |
|--------|--------------|
| **Real-time games** | Create room → player joins → game state updates → push to client |
| **Customer support chat** | Open conversation → assign agent → relay messages → push conversation state |
| **Order workflow** | Accept order → process in stages → change state → notify client |
| **Delivery/dispatch** | Request dispatch → assign/accept a driver → track state → push in real time |

There's one common shape — role-specific server processes talk in typed messages, and the
client receives state changes over a real-time connection (stream).

<iframe class="zlink-diagram" src="/common/diagrams/guide-topology-en.html"
        title="Servers talk in typed messages by role; clients receive over a stream" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-topology-en.html" target="_blank">↗ Open larger</a></p>

Each server process is an independent executable connected to the others over TCP. HTTP
ingress, the communication path to other servers, client connections, and state-unit
management all live inside one server. `samples/TicTacToe` (2 servers) and `samples/Bingo`
(4 servers) are complete working examples.

---

## Core Capabilities

### Channel Messaging — typed request-reply between servers

A channel is a name given to a communication path between servers. One side sends a request
by channel name, and the other side processes it and replies. `struct`s are exchanged
directly, and the framework handles serialization (JSON / MessagePack / Protobuf).

```cpp
// Sending side (channel client)
auto result = co_await _client
    .request ("support.core", open_conversation_req_t{user_id})
    .async<open_conversation_res_t> ();

// Receiving side (channel server handler)
class open_conversation_handler_t {
  public:
    using request_type = open_conversation_req_t;
    using reply_type   = open_conversation_res_t;
    static constexpr const char *topic_name = "OpenConversation";
    open_conversation_res_t handle (const open_conversation_req_t &req) { ... }
};
```

Besides request-reply, it also provides fanout (pub/sub) and route mesh (address routing)
patterns. [Channel messaging →](20-channel-messaging.en.md)

---

### SPOT — managing a state unit without locks

A SPOT is an execution unit that binds **"one state region"** — a game room, a support
conversation, an order-processing unit — together with its participants. Everything that
happens inside one SPOT — participant packets, timers, join/leave — is processed
**serially**. State can be accessed without `std::mutex`, and even with coroutine-based
async processing, two requests never overlap within the same SPOT.

```cpp
class conversation_spot_t : public zlink::framework::spot_t,
                             public conversation_t   // owns conversation state directly
{
  public:
    void configure (zlink::framework::spot_context_t &context)
    {
        context.handlers ().add_actor_packet<&conversation_spot_t::send_message> ();
    }

    send_message_res_t send_message (const user_actor_t &actor,
                                     const zlink::framework::message_context_t &,
                                     const send_message_req_t &request)
    {
        return append (actor.user_id, request.text);   // safe without std::mutex
    }
};
```

It splits into the entry spot (one per node), responsible for assignment/placement, and the
room spot (one per unit), the state body itself. Periodic work is registered as a timer.
[Spot →](21-spot.en.md)

---

### Stream + Actor — real-time client connections

A client's real-time bidirectional connection is called a **stream**, and the server-side
object that represents one connection is an **actor**. When a client connects, the session
creates an actor, and the actor joins a SPOT to participate in state processing.

```cpp
class support_session_t : public zlink::framework::packet_stream_session_t {
  public:
    task_t<void> on_packet (stream_t &stream,
                            const stream_dispatch_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        auto actor = co_await _actors.find (actor_id);
        co_await actor.value ().relay (payload);   // forward the current dispatch's packet to the actor
    }
};
```

The client-side connection is handled by a separate deliverable, the stream connector.
[Chapter 8 →](24-actor-session.en.md) · [Chapter 9 →](23-stream.en.md)

---

### HTTP Hosting — a REST API embedded inside the server process

Host REST endpoints in the same process without a separate web server. You can implement
path parameters and authentication logic in middleware/handlers, TLS is supported, and
readiness / liveness / health-check endpoints register in one line.

```cpp
options.http ()
  .listen ("https://0.0.0.0:8443")
  .configure_tls ([] (auto &tls) {
      tls.certificate_file (cert_path).private_key_file (key_path);
  })
  .map_post<create_game_http_handler_t> ("/games")
  .map_get<get_room_http_handler_t> ("/rooms/{room_id}")
  .map_readiness ("/ready");
```

[Chapter 20 →](42-http-hosting.en.md)

---

### Configuration · DI · Logging · Monitoring

Built-in support for what a production server needs.

- **Configuration** — composes CLI arguments, environment variables, and a JSON file in
  priority order. One call to `bind<T>()` maps a settings section onto a struct.
- **DI container** — handler constructor parameters get services auto-injected. Supports
  singleton / scoped / transient lifetimes.
- **Logging** — inject a `logger_t<TOwner>` via DI and get logs auto-tagged with the source
  name.
- **Monitoring / Health** — receive socket, discovery, spot, and timer events as typed
  subscriptions. Wire health checks to `/ready` and `/healthz` endpoints.

[Chapter 18 →](40-di-container.en.md) · [Chapter 19 →](41-configuration.en.md) · `11. Monitoring` chapter

---

### Registry / Discovery — automatic server address wiring

When several Play server instances come up, you don't hardcode which server to connect to or
its endpoint. A shared Redis-backed Location Store tracks addresses, and each server discovers
them dynamically through `add_location_store<redis::redis_location_store_t> ()`.

```cpp
options.add_location_store<redis::redis_location_store_t> ()
  .set_connection_string (topology.redis_endpoint)
  .set_key_prefix (topology.redis_key_prefix + "location:");

auto room_mesh = options.add_route_mesh (sample_names_t::room_spot_mesh);
room_mesh.set_routing_id (zlink::routing_id_t::from ("bingo-play-" + topology.play_node))
  .listen (topology.selected_play_spot_router_endpoint ());
room_mesh.objects ()
  .server ()
  .add_entry_spot<bingo_entry_spot_t> ()
  .add_spot_factory<bingo_room_spot_t> (sample_names_t::room_spot);
```

[Chapter 10 →](25-location.en.md)

---

## Table Of Contents

| Order | Document | Content |
|----|------|------|
| 1 | [Overview](01-overview.en.md) | A quick map of the core surface, layers, and topology |
| 2 | [Quickstart](../../quickstart.en.md) | Install, a minimal project where two processes call each other, first-run checks |
| 3 | [Core Concepts](03-concepts.en.md) | What a channel, a Spot, an Actor and a session each are |
| 4 | [Channel Messaging](20-channel-messaging.en.md) | The path that calls by name — registering and calling |
| 5 | [Spot](21-spot.en.md) | Creating and calling a shared place by id |
| 6 | [Actor](22-actor.en.md) | Creating and calling one entity by id |
| 7 | [STREAM](23-stream.en.md) | A client outside the mesh attaching over one connection |
| 8 | [Session and Actor](24-actor-session.en.md) | Binding one connection to one Actor |
| 9 | [Location](25-location.en.md) | Looking up the node something is on by id |
| 10 | [Monitoring](26-monitoring.en.md) | A placeholder in the feature guide — no body yet |
| 11 | [The Execution Model](32-execution-model.en.md) | Two queues, the serialization scope, the turn |
| 12 | [Backpressure](33-backpressure.en.md) | When arrival outruns processing, and the options that affect it |
| 13 | [Activation and Lifetime](34-activation-lifetime.en.md) | Creation time per kind, lifecycle callbacks, injection lifetime |
| 14 | [Actor Membership](35-actor-membership.en.md) | Moving between Spots, reservations and limits |
| 15 | [Timers and Workers](36-timer-worker.en.md) | Periodic execution, running outside the line, giving the turn back |
| 16 | [Relocation](37-relocation.en.md) | What survives a move, the adapter, the unit |
| 17 | [How Channels Work](30-channel-patterns.en.md) | Pattern differences, target selection, pub/sub, connection and discovery |
| 18 | [Handlers and Message Processing](31-handler-dispatch.en.md) | Registration variants, filters, codecs, handler kinds |
| 19 | [How STREAM Works](38-stream-boundary.en.md) | Startup checks, error ownership, reply tokens, execution mode |
| 20 | [How Session Binding Works](39-session-binding.en.md) | How many bindings, route refresh, disconnect, failures |
| 22 | [Operations and Lifecycle](12-operations.en.md) | Runtime metrics, relocate, drain, readiness wiring |
| 23 | [Options](16-options.en.md) | The option list, the defaults and when to change them |
| 24 | [Picking a Sample](14-samples.en.md) | Choosing which sample to read first and how to run it |
| 25 | [Reading Along: Bingo](50-bingo.en.md) | Authentication, matching, rooms, timers, multicast and cleanup in code order |
| 26 | [Reading Along: TicTacToe](51-tictactoe.en.md) | Manual connection and registration, an Actor join onto another node |
| 27 | [Reading Along: SupportChat](52-supportchat.en.md) | Several Actors on one session, metadata relay, the idle timer |
| 28 | [Reading Along: DeliveryDispatch](53-deliverydispatch.en.md) | One-way sends, deadline records and reassignment, customer pushes |
| 29 | [Reading Along: ShoppingMall](54-shoppingmall.en.md) | The owner Instance Spot, replay, next step and expected version |
| 30 | [Reading Along: GameQuest](55-gamequest.en.md) | A per-player owner, best-effort pushes and reconciliation |
| 31 | [Reading Along: ZoneWorld](56-zoneworld.en.md) | Capacity placement, boundary joins and relocation, fanout and observation |
| 32 | [E2E Testing](15-e2e-testing.en.md) | Verifying the whole system with the client library |
| 33 | [Key Type Index](13-interface-catalog.en.md) | The contract interfaces indexed by their verification code |
| 34 | [DI Container](40-di-container.en.md) | ZLink host — the three lifetimes, registration, handler injection |
| 35 | [Configuration](41-configuration.en.md) | ZLink host — sources, precedence, section binding |
| 36 | [HTTP Hosting](42-http-hosting.en.md) | ZLink host — the embedded HTTP server and route handlers |
| 37 | [Execution and Configuration Model](43-execution-model.en.md) | ZLink host — where the thread model meets configuration |
| 38 | [Monitoring](26-monitoring.en.md) | Awaiting rewrite — status snapshots and diagnostics |

The file number identifies the same chapter regardless of language. Chapters 1–17 are shared
across all five languages, and chapters 18–21 are C++-only — DI, configuration, and HTTP
hosting, which .NET gets from its runtime, are provided directly by the framework in C++, as
is the execution model.

These four are foundational, so after reading chapters 2 and 3 you can jump straight to 18–21
and come back to chapter 4.

---

## How To Read The Diagrams

Every diagram in this guide uses the same visual language — color maps to concept.

<iframe class="zlink-diagram" src="/common/diagrams/guide-element-kinds-en.html"
        title="The five kinds in the diagram" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-element-kinds-en.html" target="_blank">↗ Open larger</a></p>

Several chapters draw the same TicTacToe/Bingo topology, and only the zoomed-in location
changes per chapter.

## Related Documents

- The HTTP **client** (the side that sends requests) is a separate deliverable —
  [zlink::http_client user guide](../http-client/README.en.md)
- The design contract (draft) lives in [doc/spec/](../../README.en.md). When it conflicts
  with the guide, the code and the spec are authoritative.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
