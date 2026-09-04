---
title: "10. Location — Auto-Connect and Object Location · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/10-location.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 9. STREAM](09-stream.en.md) | [Next: 11. Monitoring — Status Observation And Diagnostics](11-monitoring.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C#/.NET](../../../dotnet/guide/server/10-location.en.md) · **C++** · [Java](../../../java/guide/server/10-location.en.md) · [Kotlin](../../../kotlin/guide/server/10-location.en.md) · [Node/TypeScript](../../../node/guide/server/10-location.en.md)
<!-- language-switch:end -->

# 10. Location — Auto-Connect and Object Location

> **The documents that own this chapter's contract** — defined by
> [Location runtime](../../../common/spec/server/05-location-relocation/01-location-runtime.en.md),
> [Location Store](../../../common/spec/server/05-location-relocation/02-location-store-redis.en.md), and the
> [per-language location public contract](../../../common/spec/server/languages/README.en.md).
> This document explains how the application registers a Store and checks status.

## 0. What It Provides

The Location Store stores the MeshNode descriptor and the current owner of each Actor/Spot.
The Framework uses this information to auto-connect peers and deliver by logical ID to the
current owner.

<iframe class="zlink-diagram" src="/common/diagrams/10-location-en.html" title="Location — Registering and Looking Up Location" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/10-location-en.html" target="_blank">↗ View larger</a></p>

The Store is used only to look up locations. The actual application message is sent directly to
the selected MeshNode.

## 1. Registering a Store

The official Redis extension provides the Location Store and Relocation Store as separate
classes. The Location Store handles atomic changes to a small location record. The
Relocation Store keeps residual relocation records — Instance Spot cold-activation records
and the terminal records of pending requests that complete after a relocation. The moving
state, queue, and timers themselves never pass through the store; they travel directly from
the source to the target over the mesh connection.

```cpp
// Register the Store that decides the current owner and location.
options.add_location_store (
  std::make_shared<framework::redis::redis_location_store_t> (
    framework::redis::redis_location_options_t{
      .connection_string = "redis-host:6379",
      .key_prefix = "game:location"}));

// Register a separate Store that keeps residual relocation records (Instance Spot activation, pending-request terminals).
options.add_relocation_store (
  std::make_shared<framework::redis::redis_relocation_store_t> (
    framework::redis::redis_relocation_options_t{
      .connection_string = "redis-host:6379",
      .key_prefix = "game:relocation"}));
```

Both Stores can use the same Redis deployment. Keep the key prefixes different. The Framework
doesn't rely on a cross-store transaction, so you can also split them onto separate physical
Redis instances if you need to.

Once a Store is registered, the application doesn't call the provider's operations or
dispose of it directly. The Framework manages the Store's lifetime and call order.

**Configuration determines which Stores are required.**

| Store | When it's required |
| --- | --- |
| Location Store | **Required** for a MeshNode whose Object role is `Client` or `Server`. Without it, startup ends in a configuration error before the socket opens |
| Relocation Store | **Required** if **at least one** factory or Instance Spot factory has a relocation policy. It can be omitted only if relocation is disabled everywhere and there is no Instance Spot factory |

**Register each exactly once.** There's no API that bundles both into one registration
call, and missing one that's needed, or registering either more than once, is a
configuration error before the socket binds. The Framework doesn't create a fallback Store
inside the process when one is missing — **it fails instead of silently running as a single
node.**

## 2. Auto-Connect

MeshNodes sharing the same Location Store confirm each other's endpoint and role through the
descriptor. In an Automatic RouteMesh, only the side with the smaller RID initiates the
connection. If connection contention produces a duplicate candidate, handshake and admission
keep only one candidate Ready.

```cpp
auto play = options.add_route_mesh ("play");
play.listen ("tcp://0.0.0.0:5501").set_routing_id (zlink::routing_id_t::from (std::string ("play-1")));

play.set_object_role (object_role_t::server)
  .add_spot_factory<room_spot_t> (
    "room",
    [] (spot_context_t context) { return std::make_shared<room_spot_t> (std::move (context)); },
    [] (auto &factory) { factory.recreate_on_relocation (); });

play.channel_name ("play.ops").server ()
  .add_request_handler<node_status_handler_t, get_node_status_t, node_status_t> ();
```

The application never specifies the Node RID or endpoint on which to create an Actor/Spot.
The Framework checks the stable type, Serving status, capacity, and placement weight to
select an eligible node.

A host that uses even one manual peer doesn't support host relocation. Don't mix automatic
and manual connections on the same MeshNode.

## 3. Location Options

`configure_locations()` sets the lease, route cache, and message-follow windows.

```cpp
auto &location = options.configure_locations ();
location.owner_lease_renew_interval = std::chrono::seconds (5);
location.owner_lease_ttl = std::chrono::seconds (15);
location.message_follow_duration = std::chrono::seconds (30);
```

| Option | Default | Meaning |
|---|---:|---|
| `owner_lease_renew_interval` | 5s | The owner lease renewal interval |
| `owner_lease_ttl` | 15s | Time after which an owner with stalled renewal is judged expired |
| `owner_lease_renew_timeout` | 3s | The maximum time to wait for one renewal request |
| `owner_lease_fencing_margin` | 5s | Margin to cut off new work ahead of expiry |
| `polling_interval` | 1s | The interval to re-read the Store when there's no change watch |
| `store_failure_grace` | 30s | How long the last route decision is kept during a Store outage |
| `route_cache_max_age` | 15s | The max time before a cached route is re-checked |
| `message_follow_duration` | 30s | How long the previous owner relays messages to the new owner during a move |

There are no separate settings for relocation participants or records. The share of the mesh connection
that moving state may use is tuned with the chunk-size and in-flight-budget
server settings; the setting list and tuning criteria are covered by
[12-operations §2](12-operations.en.md#2-relocate--moving-to-another-host-while-keeping-state).
Target staging uses the host's shared Application Job Queue reservation and processes any backlog
beyond the live-job limit gradually. Core memory accounting and the negotiated
frame size still apply; see
[Relocation Flow](../../../common/spec/server/05-location-relocation/04-relocation-flow.en.md).

**The four lease values are tied together.** Breaking the following relationship is a
startup error. Look at all four together when changing any one value.

```text
renew interval + renew timeout < owner lease TTL - fencing margin
```

At the defaults, `5 + 3 < 15 - 5` holds. Shrinking only the TTL, or only growing the renewal
interval, breaks this inequality. Every value must be positive.

**A Store outage affects route availability and owner eligibility differently.** `store_failure_grace` is how long the
last fully-read node list is kept — it is **not** how long owner eligibility is extended.

| During grace | Result |
| --- | --- |
| An already-established connection | Status judgment continues |
| A new outbound connection | Not made. Even after grace ends, not made until the entire node list can be re-read at one consistent point in time |
| Owner lease / relocation deadline | **Not extended** |

The moment a host whose lease renewal has stalled crosses its computed deadline, it **stops
accepting new work** — state-changing messages, starting a timer, finalizing a factory/restore
result, changing relocation state, and reserving admission capacity are all blocked. Work
already in the queue still finishes and cleans up. This is the mechanism that keeps a new
owner and an old owner from writing at the same time.

The Capture/Restore callback ceiling follows the
[per-language location option contract](../../../common/spec/server/languages/README.en.md).

## 4. Readiness and Operational Queries

Operational code uses the location readiness API to check whether a needed peer is Ready.
It uses the location runtime query for overall status and paged topology.

```cpp
auto status = co_await query.get_status ();
auto page = co_await query.list_topology (
  location_topology_filter_t{.mesh_name = "play"},
  location_page_request_t{.page_size = 100});

auto object_peer_ready = co_await readiness.is_peer_ready ("play", location_role_t::spot);

// Assemble status.store_healthy · status.owner_lease_healthy · object_peer_ready · page.items
// into the operational endpoint's response.
```

The operational query returns only health and topology intended for human inspection. Store keys,
authority versions, owner tokens, and relocation records are internal Framework information
and aren't returned. `NodeRid` is used only to map operational info back to the actual
transport node.

## 5. Looking Up an Actor and Spot

Business code uses the global ActorId and SpotId. The Manager's `find(...)` returns only an
object that is currently Ready.

```cpp
auto actor = co_await actor_manager.find ("player-1");
auto room = co_await spot_manager.find ("room-42");

if (room) {
    co_await spot_outbound.request_to_spot (room.value ().spot_id (), get_room_state_t{})
      .async<room_state_t> ();
}
```

Ordinary messaging doesn't use `SpotRef.NodeRid` as the target. The spot client and actor
client confirm the current owner from the Location Store, and apply Message Follow rules
while a move is in progress. `SpotRef` and `ActorRef` are used to close or delete the exact
generation they identify.

## 6. Related Documents

- Runnable verification examples for this chapter's contract: `13. Interface Catalog`
  chapter §6 — the verification class `LocationContracts`
- The formal contract: [Location runtime](../../../common/spec/server/05-location-relocation/01-location-runtime.en.md)
- Manual connection without auto-connect:
  [05-channel-messaging §6](05-channel-messaging.en.md#6-connection-control)
- Host relocate and drain observability: [12-operations](12-operations.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=Math.max(d.body?d.body.scrollHeight:0,d.documentElement?d.documentElement.scrollHeight:0);if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
