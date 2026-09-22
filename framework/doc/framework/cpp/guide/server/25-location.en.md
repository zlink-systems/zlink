---
title: "Location · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/25-location.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Location

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Session and Actor](24-actor-session.en.md) | [Next: Monitoring](26-monitoring.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/server/25-location.en.md) · [Java](../../../java/guide/server/25-location.en.md) · [Kotlin](../../../kotlin/guide/server/25-location.en.md) · [Node/TypeScript](../../../node/guide/server/25-location.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can ask which node a Spot or Actor is on right now, holding nothing but its id.
    The code comes from the [tutorial README in the examples repository](https://github.com/zlink-systems/zlink-cpp-examples/blob/main/tutorial/README.md); follow its Download, Build, and Run sections to reproduce the results below.

[Spot](21-spot.en.md) and [Actor](22-actor.en.md) were called by id alone, and the Framework
found which node they were on. The place that holds that record is the **Location Store**. This
chapter covers reading that record from an application; what the Store writes down, and how
processes find each other through it, is covered by
[How Channels Work](30-channel-patterns.en.md#61-location-store--where-who-is-where-is-written-down).

## 1. Lookup Against Call — a Lookup Changes No State

A call makes the target do something. A lookup **reads the location without touching the
target.** Use it to show which server a room is on right now on an operations screen, or to
confirm that the record changed after a target was moved.

<iframe class="zlink-diagram" src="/common/diagrams/25-location-find-en.html" title="find asks the Store and stops there" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/25-location-find-en.html" target="_blank">↗ View larger</a></p>

What comes back is the record as it stands at that moment. It does not confirm that the target
actually answers, so **when you have to know whether it is alive, send a request rather than a
lookup.**

## 2. The Store Requirement on a Node That Looks Up

A lookup reads the Store, so the process doing the lookup has to have that Store registered
**under the same prefix**. The registration code is the one in
[Spot](21-spot.en.md#2-the-location-store--a-prerequisite-for-registering-a-spot).

A node that runs neither Spots nor Actors and only calls them needs the Location Store alone.
The Relocation Store is required only on a node that registered a factory.

## 3. Asking Where an Id Is

A Spot is asked of the spot manager and an Actor of the actor manager. Both take the id alone,
and neither names a node to ask.

```cpp
--8<-- "framework/languages/cpp/tutorial/Client/main.cpp:location-find"
```

What comes back is one reference carrying the id and the node name, and an empty result when
nothing is found. A lookup answers **only for a target that can receive a message now**, so a
target still being created, or in the middle of a move, comes back empty as well. One empty
result is therefore not a verdict that no such id exists.

## 4. What You See When You Run It

With the tutorial Server and Client running as the README's Run section specifies, send these `curl` requests to the Client's HTTP surface: each HTTP response appears on `curl` stdout, and the creation and lookup handler records appear on the Server process's stdout or in `server.log`.

```bash
curl -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "5ce8339b-ec20-42d8-a117-a74677ad9af0"

curl http://127.0.0.1:5080/locations/rooms/5ce8339b-ec20-42d8-a117-a74677ad9af0
# {"spotId":"5ce8339b-ec20-42d8-a117-a74677ad9af0","generation":1,"node":"game-server-1"}

curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"

curl http://127.0.0.1:5080/locations/players/p7
# {"actorId":"p7","generation":1,"node":"game-server-1"}

curl http://127.0.0.1:5080/locations/players/ghost
# 404
```

Both the room and the player answered `game-server-1`. The last call asked for an id that was
never created, and the empty result became a 404.

## 5. The Scope of a Lookup Result — a Node Name Is Not a Call Address

The node name a lookup returns is **the location at that instant**, not a call address. A call
gives the id alone, exactly as in [Spot](21-spot.en.md) and [Actor](22-actor.en.md). The
Framework reads the same record again on every call, so a target that moves to another node
after the lookup is still reached. Take the node name and send through
[calling a node directly](20-channel-messaging.en.md#36-calling-a-node-directly) and that
following is lost.

There is a place where the reference itself is passed on. A call that closes or destroys takes
only the [generation that reference points at](22-actor.en.md#33-generation-in-a-reference) and
leaves a target recreated under the same id untouched.

## 6. Related Documents

- State objects called by id — [Spot](21-spot.en.md) · [Actor](22-actor.en.md)
- What the Store writes down — [How Channels Work](30-channel-patterns.en.md#61-location-store--where-who-is-where-is-written-down)
- Operational queries — [Operations and Lifecycle](12-operations.en.md#6-location-readiness-and-operational-queries)
- A running version of this chapter's code — [the tutorial README in the examples repository](https://github.com/zlink-systems/zlink-cpp-examples/blob/main/tutorial/README.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
