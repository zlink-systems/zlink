---
title: "MeshNode"
---

# MeshNode

[Spec table of contents](README.en.md) · [Previous: Spot Messaging](12-spot-messaging.en.md) · [Next: Actor Model](14-actor-model.en.md)

> **What this chapter defines** — the identity, object role, object
> placement conditions, and startup order of a MeshNode participating in
> a RouteMesh.


## 1. Scope This Document Defines

This document defines, for a MeshNode participating in a RouteMesh in
ZLink Framework, its identity, object role, the conditions under which it
can place an object, and the startup order.

A [MeshNode](01-glossary.en.md#meshnode) is physically connected to other
nodes and provides Channel membership. Actor and Spot use this
connection but don't use the MeshNode RID as their own logical identity.
The framework links the global logical identity of an Actor and
[Spot](01-glossary.en.md#spot) to the route of the MeshNode where the
current owner exists.

## 2. Identity And Configuration A MeshNode Has

The following information belongs to one MeshNode.

| Item | Meaning and mutability |
|---|---|
| `MeshName` | Determines which physical [RouteMesh](01-glossary.en.md#routemesh) and MeshNode descriptor namespace it belongs to. Can't be changed after the MeshNode starts. |
| Routing ID (`RID`) | The transport identity that identifies the current MeshNode lifecycle. |
| Endpoint | The address other peers connect to on this MeshNode's ROUTER. |
| `ChannelName` set | The list of Channels it participates in with a Server role. Zero or more can be registered, and can't be changed after starting. |
| Object role | One of `None`, `Client`, `Server`. Fixed before startup. |
| Lifecycle generation | A non-zero identifying value distinguishing different lifecycles on the same transport identity. |
| [Descriptor](01-glossary.en.md#descriptor) revision | A non-zero value distinguishing [MeshNode descriptor](01-glossary.en.md#meshnode-descriptor) content that can change within the same lifecycle. |

`MeshName` isn't included in the identity of an ActorId or SpotId. ActorId
and User/Instance SpotId are each a global key across the whole
transaction scope the Location Store manages. `MeshName` is an attribute
representing the Mesh where an object is first placed and the physical
route by which the current [owner](01-glossary.en.md#owner) is reached.

Only one MeshNode with the same `MeshName` can be registered in the same
process. Multiple MeshNodes with different `MeshName`s can be registered,
but the framework doesn't automatically build a transport relay between
RouteMeshes.

Adding a `ChannelName` doesn't create a separate socket or endpoint.
After a MeshNode descriptor is published, the following settings can't be
changed.

- Channel [membership](01-glossary.en.md#membership)
- Object role
- Factory
- Stable type and type capability

## 3. Routing ID

### 3.1 The RID Used By Automatic Discovery

For a MeshNode using automatic discovery, the framework generates a new
RID for each lifecycle. The caller can only specify a prefix used for
diagnostics. If the prefix is omitted, the framework uses a default
prefix matching the listener kind.

| Component | Contract |
|---|---|
| Prefix | Only ASCII `[A-Za-z0-9._-]` characters, length `1..64`. |
| UUID | A 16-byte random value using the RFC 4122 UUID v4 bit layout, represented as a 36-character lowercase canonical string in `8-4-4-4-12` digit groups. |
| Full RID | The format `prefix-<lowercase-canonical-uuid-v4>`, UTF-8 encoded size at most 255 bytes. |

The full rule covering the Core binary RID, Framework prefix, Entry Spot,
and caller-provided RID together is defined by
[System-Wide Routing ID Policy](10-network-listener-identity.en.md#7-system-wide-transport-rid-and-spot-id-policy).

Don't interpret the prefix and UUID as application identity that persists
across object placement, sharding, or a restart.

The CAS that confirms ownership of a MeshNode descriptor checks whether
the same `(MeshName, RID)` is currently used by a different owner. If an
active conflict is confirmed, the existing descriptor isn't changed, and
a second UUID or claim isn't created. Startup ends immediately with a
configuration error.

A replacement lifecycle doesn't reuse the previous lifecycle's RID — it
generates a new RID.

### 3.2 Entry Spot ID

An Object Server MeshNode also issues an Entry Spot ID with the same
diagnostic prefix.

```text
MeshNode RID:   <prefix>-<node-uuid-v4>
Entry Spot ID: <prefix>-entry-<lowercase-canonical-uuid-v4>
```

The MeshNode and Entry Spot each generate a separate UUID v4. The
relationship isn't determined by comparing the two UUID values. The Entry
Spot ID is kept for the same MeshNode lifecycle and newly issued on a
replacement lifecycle. If an active conflict of the global Spot ID
authority is confirmed, a second UUID or reservation isn't created, and
startup ends immediately with a configuration error.

The full Entry Spot ID must be at most 255 UTF-8 bytes. If the prefix is
omitted, the same default diagnostic prefix chosen for the MeshNode's
automatic RID is also used for the Entry Spot.

The MeshNode descriptor publishes the exact Entry Spot ID together with
the lifecycle generation. Actor placement and Entry Spot join use this
mapping, and don't compute a node relationship by parsing the prefix or
the `entry` marker. The prefix and marker are diagnostic information, not
a stable host identity, shard, or placement key.

### 3.3 Fixed RID

Fixed RID is only allowed in an explicit manual topology that doesn't use
the [Location Store](01-glossary.en.md#location-store)'s MeshNode
descriptor and [automatic discovery](01-glossary.en.md#automatic-discovery).

Setting a fixed RID on a MeshNode whose object role is `Client` or
`Server`, or setting automatic mode and fixed RID together, is a startup
configuration error.

## 4. Object Role And The Features That Can Be Registered

Each MeshNode selects one object role.

| Object role | Logical object operation | Local [factory](01-glossary.en.md#factory) and Entry Spot | Target to place a new object on |
|---|---|---|---|
| `None` | Doesn't provide object operations. | Not created. | Excluded from candidates. |
| `Client` | Can start object create, find, and message. | Not created. | Excluded from candidates. |
| `Server` | Includes everything `Client` provides. | Hosts objects of registered types and the Entry Spot. | Becomes a candidate if the registered type and other placement conditions match. |

`Client` and `Server` roles require a Location Store. Selecting `None`
doesn't create an object manager, factory, placement feature, or hidden
local object runtime. Factory and Entry Spot can only be registered on
the `Server` builder. The Entry Spot ID is issued by the framework — the
caller can't generate it or specify a fixed RID for it.

An Object Client can't register a Spot/Actor factory or an application
Node direct handler. A RouteMesh Channel Server independent of object
features can be registered on the same MeshNode. This combination isn't
an object placement target, but it is a Channel target.

Only when both MeshNodes are Object Client and neither has RouteMesh
Channel Server membership is a peer connection not made. If either side
has Server membership, it connects even if weight is `0`. A pair with
only Channel Client membership doesn't connect.

The following .NET excerpt is an example for understanding common role
selection and factory registration. It doesn't require the same
signature in other languages; the exact .NET contract is defined by the
[.NET Configuration Interface](server/languages/dotnet/interfaces/03-configuration-topology.en.md).

```csharp
public interface IZLinkMeshNodeBuilder
{
    IZLinkMeshNodeBuilder SetPlacementWeight(int weight);
    IZLinkMeshObjectRoleBuilder Objects(); // starts object role selection.
}

public interface IZLinkMeshObjectRoleBuilder
{
    IZLinkMeshObjectClientBuilder Client(); // can only start object operations.
    IZLinkMeshObjectServerBuilder Server(); // can also register factory and Entry Spot.
}

public interface IZLinkMeshObjectServerBuilder
{
    IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
        where TEntrySpot : class, IZLinkEntrySpot;
    IZLinkMeshObjectServerBuilder AddActorFactory<TActor, TFactory>(
        string actorType,
        Action<IZLinkActorFactoryBuilder<TActor>> configure)
        where TActor : class, IZLinkActor
        where TFactory : class, IZLinkActorFactory<TActor>;
}

public interface IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    IZLinkActorFactoryBuilder<TActor> DisableRelocation();
    IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation();
    IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>;
}
```

```csharp
var mesh = options
    .AddRouteMesh("world")
    .SetPlacementWeight(100); // weight used only for placement selection of a new object.

mesh.Objects()
    .Server()
    .AddActorFactory<PlayerActor, PlayerActorFactory>(
        "player",
        factory => factory.RecreateOnRelocation());
        // fixes stable type, factory, and relocation policy together in one registration.
```

When registering an Actor, User Spot, or Instance Spot factory, the
following two values must both be specified.

- [Stable type](01-glossary.en.md#stable-type), UTF-8 `1..255` bytes
- One relocation policy among `DisableRelocation`, `RecreateOnRelocation`,
  `PreserveStateWith`

The framework synchronously runs the configure callback exactly once
inside the factory registration call. If the callback returns normally,
the configuration is fixed, and calling the retained builder again
afterward is a configuration error. If the callback throws, that factory
isn't registered and the exception is propagated to the caller.

Stable type is a case-sensitive exact value. The framework doesn't apply
normalization and doesn't use a language class FQN as wire or Store
identity. Registering the same `(object kind, stable type)` combination
twice is a startup error.

An overload that omits the relocation policy, or a compatibility
default, isn't provided.

## 5. Object Placement Capability

An Object Server's MeshNode descriptor includes a node-wide placement
weight, Actor/Spot capacity projection, and capability per registered
type.

### 5.1 Weight And Capacity

| Item | Contract |
|---|---|
| Placement [weight](01-glossary.en.md#weight) | Range `0..10000`, default `100`. Independent of Channel weight. An out-of-range value is a configuration error at startup config and at runtime change. |
| Per-node Actor limit | Default `0` means no limit. If positive, it's the maximum Actor count in range `1..2^31-1`. A negative value is a startup configuration error. |
| Per-node Spot limit | Default `0` means no limit. If positive, the range is `1..2^31-1`, summing User Spot and Instance Spot. A negative value is a startup configuration error. |
| Per-Spot-stable-type limit | Default `0` means no limit. If positive, the range is `1..2^31-1`, applying to that User/Instance Spot type. A negative value is a startup configuration error. |
| Entry Spot | Fixed at one per Object Server node, excluded from the configurable Spot limit. |
| Pending activation | Default `128`; limits concurrently in-progress activation admission, not object population. |

When creating a new object or moving an existing object to a different
node, the framework selects the target MeshNode. A MeshNode with
placement weight `0` is excluded from new-target candidates for these two
operations.

A message sent to an object that already exists on that MeshNode isn't
an operation that selects a new target, so it isn't blocked by this
weight alone. Changing weight to `0` doesn't cancel a reservation already
confirmed.

The framework first checks the configured Actor/Spot limit by summing
active count and reserved slots, then applies weight. A limit of `0`
skips the check. If no node satisfies the capacity condition, it's
`CapacityExceeded`. The descriptor's count is a projection for candidate
selection — the Location Store's atomic reservation is the final
judgment. The sum of positive placement weight across remaining
candidates is computed using at least a 64-bit integer so it doesn't
overflow.

The startup builder, runtime option, MeshNode descriptor, and monitoring
snapshot use the same weight and capacity values.

### 5.2 Conditions For Selecting A Target Node

A logical create's caller doesn't specify a target RID, predicate, or
placement callback. Even when `InMesh` is specified, it only selects the
Mesh to search for candidates in, not the target node.

The framework selects the target using the following conditions.

1. Confirms the node is in `Serving` state.
2. Confirms the current owner lease is valid.
3. Confirms the requested object kind and stable type are registered.
4. Confirms active/pending capacity remains.
5. Applies node-wide placement weight across the remaining candidates.

The framework reserves a slot to place the object on the selected node so
it doesn't double-use capacity with another creation operation. It
doesn't require the application to select a target RID or owner token.

If `GetOrCreate` found an already-Ready object, it doesn't reapply the
current owner's capacity and weight.

## 6. Registration And Startup Order

The framework starts a MeshNode in the following order.

1. Validates `MeshName`, object role, routing mode, endpoint, Channel
   set, factory, stable type, relocation policy, factory option, and
   capacity.
2. If a role requiring the Location Store, acquires the host
   [owner lease](01-glossary.en.md#owner-lease) and completes the
   automatic RID's MeshNode descriptor owner CAS.
3. Binds the ROUTER, then fixes the actual endpoint to publish to other
   peers.
4. Publishes the MeshNode descriptor containing all necessary
   information and computes which peers to connect to.
5. After finishing peer admission and local handler/object runtime
   preparation, publishes `Serving` state and opens up new target
   selection.

A host using an object role must explicitly register a Location Store.

In manual mode, the application provides the endpoint. If the
configuration uses an expected RID, the expected RID and endpoint are
provided together. Manual mode doesn't provide an object runtime.

## 7. Peer Admission And Messaging

### 7.1 Peer Connection

The peer handshake exchanges the following information.

- `MeshName`
- RID
- [Lifecycle generation](01-glossary.en.md#lifecycle-generation)
- [Descriptor revision](01-glossary.en.md#descriptor-revision)
- The immutable `ChannelName` set
- Security identity

If `MeshName` or trust profile differ, or it's a duplicate pipe of the
same lifecycle identity, it isn't admitted.

Lifecycle generation is a non-zero opaque equality token. Which lifecycle
is newer isn't judged by numeric magnitude.

When reconnecting with a fixed RID in a manual topology, a connection of
a different generation is only included in target selection after all of
the following conditions are met.

1. The application configuration has intent to connect to that peer.
2. Authenticated connection handover has completed.
3. Service liveness confirmation has established that the previous pipe
   has ended.

In automatic RouteMesh, only the MeshNode with the smaller RID starts the
connect. If a duplicate candidate arises from manual bidirectional
connect or automatic connection competition, only one
[ready](01-glossary.en.md#ready) connection is kept, per
[RouteMesh's Duplicate Peer Connection Rule](07-channel-topology.en.md#51-automatic-only-the-meshnode-with-the-smaller-rid-starts-the-connection).

### 7.2 Channel Weight Update

The handshake also carries per-Channel weight. When Channel weight is
changed at runtime, the lifecycle generation is kept and only the
descriptor revision increases.

A peer only applies the full weight [snapshot](01-glossary.en.md#snapshot)
carrying a larger revision within the current generation. A weight change
doesn't recreate the connection or replay application messages, and
doesn't change node-wide placement weight either.

### 7.3 Target Selection Per Messaging Method

| Messaging method | How the target is selected and delivered |
|---|---|
| Node direct | Submitted once to an exact target RID within the caller-specified `MeshName`. An Object Client RID isn't an application Node direct target. |
| Channel | A process-local `ChannelName` index determines the RouteMesh. One Server that's ready in that Mesh and has Channel weight greater than 0 is selected in proportion to weight. |
| Logical Multicast | First, every remote MeshNode that's a member of that ChannelName, is ready, and has Channel weight greater than 0 is selected. Each receiving MeshNode delivers the message to every local Spot subscription matching the ChannelName and topic condition. |
| Actor direct | After confirming the global ActorId's current Ready authority, submits to the current owner route. |
| Spot direct | After confirming the global SpotId's current [Ready](01-glossary.en.md#ready) [authority](01-glossary.en.md#authority), submits to the current [owner route](01-glossary.en.md#owner-route). |

Actor/Spot direct only use the logical ID as target. Where
`ObjectGeneration` is used and where it isn't is defined by
[Spot/Actor Routing §2.5](18-object-routing.en.md#25-where-objectgeneration-is-used-and-where-its-not).

Target selection and message submit are one operation. The framework
doesn't return a list of selected RIDs to the application and then
require a separate send.

Node/Channel/Actor/Spot send and request use the same MeshNode ROUTER.
Classic fanout is a separate PUB/SUB socket contract and isn't included
in MeshNode membership.

[Node direct](01-glossary.en.md#node-direct) is used for infrastructure,
diagnostics, or manual topology where the exact `MeshName` and RID are
part of the operation's meaning. For an application request that multiple
nodes provide the same feature for, use
[ChannelName](01-glossary.en.md#channelname).

Actor and Spot messaging use the global ActorId or SpotId as target. The
caller doesn't pass NodeRid or MeshName as target.

The current `MeshName` and NodeRid of an existing Actor/Spot are provided
by the Location Store authority. Only for a Missing
[Instance Spot](01-glossary.en.md#entry-user-instance-spot) can an
optional initial Mesh and stable type be specified on the Instance intent
of a [Spot direct](01-glossary.en.md#spot-direct) fluent call. The
initial Mesh is only used for cold-activation placement, and doesn't
restrict or move an existing owner's current Mesh.

Application payload is processed serially on the owner's application
turn. Request completion and service control such as liveness/admission/
relocation/reply recovery are received on the existing Completion
connection, and send-ready is delivered via a Core callback. Internal
framework work such as location reconciliation and reservation proceeds
even while an application handler is waiting. Actor/Spot lifecycle
application callbacks run on the application turn. An application handler
isn't run directly from a transport readiness callback.

ChannelName handlers and RID direct handlers use different namespaces.

- The Channel handler context internally preserves `ChannelName` and
  reply source identity. It doesn't expose `MeshName` or physical route
  selection to business code.
- The RID direct handler context provides the direct route's `MeshName`
  and source RID.

Spot [Logical Multicast](01-glossary.en.md#logical-multicast) checks the
`(ChannelName, topic filter)` [subscription](01-glossary.en.md#subscription)
node-locally. The sending MeshNode submits a routed message once per
remote node of the target Channel. The receiving MeshNode acquires a
reference to the same immutable message storage for each matching local
Spot and puts it on the Spot queue.

## 8. Drain And Shutdown

A `Relocating` node is excluded as a target for the following new work.

- Channel selection
- Object create and membership
- Relocation

The existing owner's message and timer for a unit that hasn't yet
obtained a relocation permit keep being processed. Once per-unit seal is
finished and the source's application dispatch is fully closed, it
transitions to `Draining`.

A create whose reservation is already finished, an accepted message, a
completion, and a relocation barrier proceed to a terminal state
according to their set deadline and fence. The full shutdown and handoff
order is defined by
[Host Relocation And Shutdown](28-graceful-drain-handoff.en.md).

`Shutdown` doesn't start a new relocation. `Relocate` moves the Actor,
User Spot aggregate, and Instance Spot according to the registered
relocation policy.

Changing node weight to `0` or starting drain isn't grounds for hiding
and re-creating an existing object, or submitting an application payload
to a different owner as a new operation.

## 9. Observability Information

Runtime snapshots and events provide the following information.

- `MeshName`, RID, lifecycle generation, and endpoint
- Object role and node-wide placement weight
- Active/pending/maximum capacity
- Type capability and reservation failure
- Drain state

RID and endpoint are only used as diagnostic information, not as a metric
label. The detailed contract is defined by
[Runtime Monitoring](24-runtime-monitoring.en.md).

## 10. Implementation And Contract-Test Verification Requirements

- A duplicate `MeshName` in the same process and an invalid object role
  configuration fail at startup.
- `None`, `Client`, `Server` restrict manager, factory, and placement
  capability as contracted.
- An invalid combination of object role with Location Store, automatic
  discovery, and fixed RID fails at startup.
- Automatic RID follows the prefix and lowercase canonical UUID v4
  format, and fails with a startup configuration error on active
  conflict without a second claim.
- A replacement lifecycle uses a new RID.
- The Entry Spot ID uses the same diagnostic prefix as the MeshNode and a
  separately generated UUID v4, and the descriptor publishes the exact
  lifecycle mapping.
- A replacement lifecycle issues a new Entry Spot ID and fails
  immediately on an Entry Spot authority conflict.
- Duplicate stable type and an omitted relocation policy fail at
  startup.
- Placement weight allows `0`, default `100`, and the upper bound
  `10000`, and rejects `-1` and `10001` in both startup config and
  runtime change.
- Capacity is applied before weight, and weight `0` doesn't cancel an
  existing object or an accepted reservation.
- A Channel weight change doesn't change placement weight.
- Channel select-one reflects Channel weight and drain, and doesn't
  affect Node direct.
- Logical Multicast is sent once per remote node, and node-local Spot
  queues share immutable storage.
- ChannelName handler and RID direct handler namespaces and contexts are
  distinguished.
- A Draining node doesn't become a new placement target, and an accepted
  operation proceeds to a terminal state.
- Actor/Spot application calls don't require NodeRid or owner token as
  target.
