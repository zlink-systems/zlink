---
title: "ZLink Framework Overview"
---

# ZLink Framework Overview

[Spec index](README.ko.md) · [Previous: Framework messaging glossary](01-glossary.ko.md) · [Next: ZLink Framework interaction model](03-interaction-model.ko.md)

> **What this chapter defines** — the boundary between what the Framework
> shares with each language's service runtime (the public contract, wire
> schema, and fixtures) and what each language implements independently.


## 1. One-Line Definition

The ZLink Framework is the upper layer that connects a typed message
handler, RouteMesh, ClientServer Channel, Spot, Actor, STREAM session,
classic fanout, and the location runtime to an application host's lifecycle
and DI.

The C++/.NET/JVM/Node.js Frameworks each implement the service runtime in
their own language. This runtime uses only the installed public raw socket
API of that language's binding. What the languages share is the public
contract, the versioned protocol schema, and the verification fixtures —
they do not share a common native runtime or a service C ABI. Java and
Kotlin share one JVM runtime.

## 2. RouteMesh And MeshNode

`RouteMesh` is the physical connection scope of the MeshNodes that share
the same `MeshName`. One [MeshNode](01-glossary.ko.md#meshnode) has one
routing ID and one ROUTER endpoint. A MeshNode that provides a channel
handler participates as a Server in one or more immutable `ChannelName`s;
a call-only or Node-direct-only MeshNode can operate without any
ChannelName membership.

`MeshName` and `ChannelName` play different roles.

| Name | Meaning |
|---|---|
| MeshName | The physical mesh and RID namespace that can message each other |
| [ChannelName](01-glossary.ko.md#channelname) | A process-local logical address that selects a [RouteMesh](01-glossary.ko.md#routemesh) or ClientServer send path |

One process can have multiple MeshNodes with different
[MeshName](01-glossary.ko.md#meshname)s. Each mesh is independent and
provides no automatic relay. Adding a RouteMesh ChannelName does not add a
socket or endpoint. Within the same process, one ChannelName maps to only
one RouteMesh or ClientServer topology, and in a call role it points to
one send path of that topology.

## 3. Message Targets

Messaging on a MeshNode is distinguished by how the target is selected.

- [Node direct](01-glossary.ko.md#node-direct) designates one RID within the same MeshName.
- A channel send/request finds a process-local send path by ChannelName, and selects one ready target among that RouteMesh's members or that ClientServer's servers.
- [Spot](01-glossary.ko.md#spot) Logical Multicast targets the remote MeshNodes of a ChannelName and their node-local Spot subscriptions.
- A Spot or Actor message uses a global Spot ID or Actor ID. The Framework resolves the current Ready authority and delivers to the owner mailbox.
- Spot/Actor create and get-or-create are explicit manager operations. The Framework selects a target by object role, capacity, stable type, capacity, and node-wide weight, and returns an immutable ref after the [Ready](01-glossary.ko.md#ready) barrier.

Selection and submit are one operation. An application does not receive a
peer list or a selected RID and repeat send calls itself.

## 4. Logical Multicast And Classic Fanout

Spot [Logical Multicast](01-glossary.ko.md#logical-multicast) delivers an
event to a logical Spot whose location can change, such as a room, stage,
or zone. The sending MeshNode sends one routed message per remote MeshNode
of the target channel, and each receiving MeshNode checks its own node's
[subscriptions](01-glossary.ko.md#subscription). When multiple Spots match
on the same node, they share a reference to the immutable message storage
and each is enqueued to its own Spot queue.

Logical Multicast submits one internal ROUTER send per remote MeshNode, and
also submits independently to the local Spot queue. The HWM, send timeout,
and backpressure of a remote send follow the ordinary ROUTER rules as-is,
and a later target's failure does not cancel a submit an earlier target
already accepted.

[Classic fanout](01-glossary.ko.md#classic-fanout) is an independent
PUB/SUB capability that sends an event to a subscriber that is connected
and has finished subscription setup. A publisher using automatic discovery
publishes its actual endpoint to a dedicated location descriptor, and an
automatic subscriber connects to every live publisher of the same
ChannelName. A publisher and subscriber using only manual endpoints can be
configured without a location store. A host that doesn't need a MeshNode
or Spot can also use it, and it does not guarantee storage or replay.

## 5. Execution Owner

The Framework delivers a message to the execution unit that actually owns
the state.

| [Owner](01-glossary.ko.md#owner) | Responsibility |
|---|---|
| Node | RID direct and ChannelName handlers, node-initiated completions |
| Spot | Spot direct, Logical Multicast subscription, timers, and Spot state. An Instance Spot uses only direct and timer, not Actor [membership](01-glossary.ko.md#membership) or Logical Multicast subscription |
| Actor | Actor direct messages, Actor lifecycle, and the per-Actor mailbox |
| STREAM session | Connection lifecycle, packet dispatch, and Actor-binding ingress |

An application is not required to redistribute a Spot or Actor message
from a Node handler. The Framework service runtime drains the per-owner
bounded mailbox and connects it to the registered handler's execution
context. Transport readiness and the service protocol frame are not
exposed to an application callback.

## 6. Connection Management

Automatic discovery uses a [location store](01-glossary.ko.md#location-store)'s
[descriptor](01-glossary.ko.md#descriptor) and lease. RouteMesh looks up a
MeshNode descriptor with the same MeshName, and a ClientServer client looks
up a dedicated server descriptor with the same ChannelName. Neither
descriptor is substituted for the other. A host that uses an Object
Client/Server role or distributed discovery explicitly registers the
official Redis location store instance.

A manual peer is a connection intent the application provides, either as
an endpoint or as an expected RID plus endpoint. A manual peer also passes
the same MeshName, RID, ChannelName, generation, and security admission
checks as an automatic-discovery peer. Being manual does not change the
message path or handler meaning.

A [ClientServer Channel](01-glossary.ko.md#clientserver-channel) is a
separate service connection where a client initiates a business call and a
server provides the handler and request reply. It's used for a one-way
service boundary that doesn't need Node direct, Spot, Actor, or Logical
Multicast. The detailed roles and discovery contract are owned by
[12 ClientServer Channel](09-client-server-channel.ko.md).

## 7. What The Framework Hides

The Framework internally manages transport address selection, peer
reconnect, multipart framing, packet codec, reply correlation, and the
backpressure queue. An application handler uses a typed payload and
context, and does not wire up a raw socket itself.

Authentication, quota, WAF, public API versioning, and billing for an
external edge gateway are outside this framework's contract scope.
