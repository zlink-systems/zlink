---
title: "Framework Messaging Glossary"
---

# Framework Messaging Glossary

[Spec table of contents](README.en.md) · [Previous: Framework Public Contract Governance](00-public-contract-governance.ko.md) · [Next: ZLink Framework Overview](02-overview.en.md)

> **What this chapter defines** — the common domain terms, state names, and result names used throughout this spec.

[Spec documentation writing guide](../../../../../doc/principal/documentation/spec-writing-guide.ko.md) ·
[Spot Messaging](12-spot-messaging.ko.md)

## How To Read The Tables And .NET Code Examples

For a term denoting a value or a record, a summary table like the one below comes first.

| Item | Meaning |
|---|---|
| Shape | Indicates whether this is a single value, a closed value, a composite record, or a runtime object/state/process. |
| .NET notation | The exact type, if a formal public .NET type exists. |
| Public composition | The field, value range, and format the public contract defines. |
| Creation/management | Who creates, updates, or discards the value. |
| Lifetime | The scope in which the value is valid and the condition under which it stops being usable. |

A value whose `.NET notation` says **no public type** isn't exposed to the application as
an independent type. In that case, the C# shape shown is **contract pseudocode** meant
to illustrate structure — it doesn't name a real API or constructor. For values the
public contract defines as opaque, internal fields aren't guessed at and added.

A composite value that does have a real .NET public type shows the formal C#
declaration below the summary table. The declaration and member names use the exact
interface as-is, and each member's role is explained with a Korean comment on the
same line.

The single source of truth for the actual .NET declarations is the
[.NET Server exact interface](server/languages/dotnet/interfaces/README.ko.md) and the
[.NET Stream Connector exact interface](stream-connector/languages/dotnet/03-stream-connector.en.md).
The .NET notation in this glossary is a supplementary notation for reading the common
contract concretely.

## 1. Spot And Location

<a id="spot"></a>
### Spot

A logical instance with an address and state. It represents a target that can receive
messages — like a room, stage, or zone — and stays reachable at the same Spot ID even
when the node actually executing it changes.

| Item | Content |
|---|---|
| Shape | Stateful runtime object |
| .NET notation | `IZLinkSpot`, `IZLinkInstanceSpot`, `IZLinkEntrySpot`; the exact location snapshot is `SpotRef` |
| Public composition | Has a global Spot ID, Spot kind, stable type, ObjectGeneration, current owner, and an application queue/execution gate that depends on execution mode. |
| Creation/management | Managed jointly by the framework's Object runtime and the application's Spot implementation. |
| Lifetime | Distinguished across incarnations by ObjectGeneration; the global Spot ID stays the same even when the owner moves. |

<a id="spot-id"></a>
### Spot ID

A globally unique logical address identifying a Spot. It can't be duplicated within
the same Location Store scope, and the framework uses it to find which node the Spot
is currently on.

| Item | Content |
|---|---|
| Shape | UTF-8 string |
| .NET notation | `string` |
| Public composition | A case-sensitive exact string, UTF-8-encoded, 1 to 255 bytes. MeshName, Spot kind, and stable type aren't part of the ID. |
| Creation/management | The framework issues it for Entry Spot; the application specifies it in the manager call for User/Instance Spot. |
| Lifetime | Kept as the Spot's identity across the whole Location Store namespace. The same value can't be reused for a different Spot kind or stable type. |

Use the following `SpotRef` when you need to point to a specific Spot incarnation
together with its current location.

```csharp
public readonly record struct SpotRef(
    string SpotId,           // the Spot's global logical address
    ulong ObjectGeneration,  // the incarnation distinguishing a Spot re-created under the same ID
    string MeshName,         // the Mesh this exact incarnation belongs to
    RoutingId NodeRid);      // the owner node when this snapshot was made
```

`SpotRef` is an immutable location snapshot and doesn't own a local Spot instance or a
runtime resource. A regular Spot message takes only the Spot ID and re-resolves
current authority.

<a id="entry-user-instance-spot"></a>
### Entry Spot, User Spot, And Instance Spot

- Entry Spot has its Spot ID issued by the framework and is provided as a server
  entry point.
- User Spot is a Spot the application explicitly creates and manages.
- Instance Spot is a Spot that can be prepared on demand by its first message,
  without a separate create call.

| Kind | .NET public type | How it's created |
|---|---|---|
| Entry Spot | `IZLinkEntrySpot` | Registered at Object Server startup; the framework issues the Spot ID. |
| User Spot | `IZLinkSpot` | Explicitly created by the application via `IZLinkSpotManager.Create` or `GetOrCreate`. |
| Instance Spot | `IZLinkInstanceSpot` | Prepared by the first message when an `InstanceSpot(...)` intent is present on `IZLinkSpotSendCall`/`IZLinkSpotRequestCall`. |

The Entry Spot ID is issued in the form `<prefix>-entry-<lowercase-canonical-uuid-v4>`
for every Object Server MeshNode lifecycle. MeshNode and Entry Spot use the same
prefix but each generates its own separate UUID v4. A descriptor records the
relationship between the MeshNode and the exact Entry Spot ID, and the application
doesn't infer node relationships by parsing the Spot ID string. The RID stays the same
within the same lifecycle and a new RID is issued on a replacement lifecycle. If a
global Spot ID authority conflict occurs, startup ends immediately as a configuration
error instead of generating a new UUID or reservation. This format is reserved for
framework issuance, so if a caller specifies a User/Instance Spot ID in the same
format, it's rejected with `InvalidOperation` before the Store and factory run.

The three kinds' functionality, and the differences in Actor membership, close, and
relocation, are defined by the [Spot model](11-spot-model.ko.md).

<a id="actor-membership"></a>
### Actor Membership

The relationship indicating which Entry Spot or User Spot an Actor currently belongs
to. The Location Store holds the source of truth for this relationship. Moving an
Actor to a different Spot or node changes the Actor owner and the source/target
Spot's membership together in a single Location Store change.

This is a different concept from [Membership](#membership), which is about a node
participating in a Channel or Mesh.

<a id="user-spot-execution-mode"></a>
### User Spot Execution Mode

A startup registration option that decides which execution gate the Spot handler,
member Actor handlers, and timer callbacks share inside a User Spot.

| Mode | Execution unit | Scope that can run concurrently | `Yield` |
|---|---|---|---|
| `SpotWide` | The whole User Spot shares one single execution gate. | Runs only one of the Spot handler, member Actor handlers, timers, and lifecycle callbacks of the same User Spot at a time. Relocation moves the Spot and all member Actors as one aggregate. | Can return the shared turn. |
| `PerActor` | Separates a Spot lane, a per-Actor lane, and a per-timer lane. | Different lanes can run concurrently. Order is preserved within the same Actor and the same timer. Relocation moves Actors independently without moving Spot state. | Not usable — there's no shared Spot turn. |

`SpotWide` is the default. The mode is fixed when registering the User Spot stable
type and doesn't change during the same MeshNode lifecycle. This option doesn't apply
to Entry Spot or Instance Spot.

A `PerActor` User Spot's Spot instance provides handlers and dependencies but doesn't
own application state that survives relocation. Shared state and Spot-level schedule
that needs to survive is managed by the application in storage outside the node. On
the target, the Spot instance is re-created with the same SpotId and ObjectGeneration,
and only Actor state, Actor queues, and Actor timers are moved, per Actor.

<a id="spot-relocation-readiness-mode"></a>
### Spot Relocation Readiness Mode

A startup registration option that decides at which turn boundary a `SpotWide` User
Spot can start relocation.

| Mode | Meaning |
|---|---|
| `AnyTurnBoundary` | The framework picks a generally safe turn boundary. This is the default. |
| `ApplicationSignaled` | Only uses the boundary the application signals as safe, after the current turn, via `RelocationReady().Defer()`. |

In `ApplicationSignaled`, `Defer()` doesn't request relocation. If a relocation is
ready on the current host, that boundary is used; if not, execution continues under
the same owner. In both cases, the framework calls the `OnRelocationReadyCompleted`
callback before the next application job.

The callback is provided with a no-op default implementation in each language's Spot
interface. The application implements it only when it needs to start the next stage
of a round or match from the callback. Calling `Defer()` under `AnyTurnBoundary`,
`PerActor`, Entry Spot, or Instance Spot fails with `InvalidOperation` before the
queue changes.

<a id="meshnode"></a>
### MeshNode

A runtime node that participates in a RouteMesh to send or receive messages. A
MeshNode with the Object Server role can provide a Spot factory and lifecycle.

| Item | Content |
|---|---|
| Shape | RouteMesh runtime component |
| .NET notation | Startup configuration is `IZLinkMeshNodeBuilder`; execution-state observation is `IZLinkRouteMeshRuntime` and `ZLinkMeshNodeSnapshot` |
| Public composition | Has a MeshName, Routing ID, ROUTER listener, peer set, Channel membership, and an optional Object role. |
| Lifetime | Kept from when the host starts this RouteMesh component until drain/shutdown completes. |

<a id="routemesh"></a>
### RouteMesh

The scope in which multiple MeshNodes participate to exchange node and Channel
messages. ChannelName is used to determine which node participating in a specific
RouteMesh is a candidate to receive a message.

| Item | Content |
|---|---|
| Shape | Distributed topology distinguished by MeshName |
| .NET notation | `IZLinkFrameworkOptions.AddRouteMesh(string)` returns an `IZLinkMeshNodeBuilder`. |
| Public composition | Made up of the MeshNodes sharing a MeshName, their peer connections, the Routing ID namespace, and Channel membership. |
| Lifetime | Has a lifecycle per participating MeshNode; the framework doesn't automatically relay between different MeshNames. |

<a id="location-store"></a>
### Location Store

The storage that holds each Spot's current owner, ObjectGeneration, and lifecycle
state so multiple nodes can check it together. It also coordinates creation authority
so exactly one target is decided when a Spot is newly created.

| Item | Content |
|---|---|
| Shape | Distributed provider capability |
| .NET notation | `IZLinkLocationStore`; provides descriptor, owner lease, and authority transactions as a single provider interface. |
| Public composition | Manages descriptor, host owner lease, Spot/Actor location, durable authority, placement reservation, and generation counters. |
| Lifetime | One provider instance is registered per host. Ephemeral descriptor and durable authority follow different lifetime rules. |

<a id="object-role"></a>
### Object Client And Object Server Role

- Object Client can request Spot creation, lookup, and messaging, but doesn't
  provide a Spot factory.
- Object Server includes Client functionality and can provide a Spot factory,
  Entry Spot, and lifecycle.

Object Client is outbound-only in terms of Object functionality. It doesn't provide a
Spot/Actor factory or an application Node direct handler, but an independent
RouteMesh Channel Server can still be registered on the same MeshNode. The peer
connection is skipped only when both nodes are Object Client and neither has
RouteMesh Channel Server membership. If either side has Server membership, a
connection is required even if Channel weight is `0`.

| Item | Content |
|---|---|
| Shape | Closed MeshNode Object role |
| .NET notation | Configuration is `IZLinkMeshObjectRoleBuilder.Client()`/`Server()`; observation is `ZLinkMeshNodeObjectRole` |
| Public composition | `None`, `Client`, `Server`; Server includes Client functionality plus factory/lifecycle provision. |
| Lifetime | Fixed at MeshNode startup configuration. |

<a id="meshname"></a>
### MeshName

A name identifying one RouteMesh physical connection group. MeshNodes registered
under the same MeshName participate in the same RouteMesh. This name is also used to
specify which RouteMesh's nodes are candidates when initially placing an Object. It
isn't part of the Spot ID and isn't used to re-decide the current owner of an
already-created Spot.

| Item | Content |
|---|---|
| Shape | Logical namespace name |
| .NET notation | `string` |
| Public composition | A single string. Doesn't include a Routing ID or endpoint. |
| Creation/management | Specified by the application in RouteMesh registration and optional initial Mesh selection. |
| Lifetime | Kept as the same topology name for the duration of the RouteMesh registration. After being used for initial placement, it doesn't become part of the Spot identity or current owner. |

<a id="spot-kind"></a>
### Spot Kind

A value indicating which kind of Spot — Entry, User, or Instance — this is. The same
Spot ID can't be reused under a different kind.

| Item | Content |
|---|---|
| Shape | Closed enum |
| .NET notation | `ZLinkSpotKind` |
| Public composition | `Invalid = 0`, `Entry = 1`, `User = 2`, `Instance = 3` |
| Creation/management | Fixed by the framework based on how the Spot is registered/created. |
| Lifetime | Doesn't change for the lifecycle of the same Spot ID. |

<a id="stable-type"></a>
### Stable Type

A fixed name identifying that a Spot is the same kind even when the deployed version
or executing node changes. Used to decide which factory to use when preparing a new
Instance Spot.

| Item | Content |
|---|---|
| Shape | Case-sensitive stable name |
| .NET notation | `string` |
| Public composition | A single UTF-8, 1-255 byte string. No Unicode normalization or case folding is applied. |
| Creation/management | Specified by the application when registering a factory. It isn't automatically taken from a language class FQN. |
| Lifetime | Kept as the Spot type identity in the Store and on the wire. Can't be registered twice on the same Object Server. |

<a id="objectgeneration"></a>
### ObjectGeneration

A number distinguishing different logical incarnations of the same ActorId or Spot
ID. Used so that a previous generation's lifecycle/relocation control doesn't change
the new incarnation's state. A regular Actor/Spot message targets the current Ready
object the logical ID points to, so it doesn't use `ObjectGeneration` as a target
match condition. Even when the application object is re-created on the target during
relocation under the `RecreateOnRelocation` policy, this value is kept because it's
still the same logical incarnation continuing.

| Item | Content |
|---|---|
| Shape | An increasing generation value |
| .NET notation | `ulong` |
| Public composition | A single integer in the range `1..long.MaxValue`. Represented as a decimal string in JSON. |
| Creation/management | Issued by the Location Store provider's transaction-domain global counter. |
| Lifetime | Kept for the duration of the same logical incarnation. Unchanged across same-node Join, cross-node relocation, and `RecreateOnRelocation`. A new value is issued when a new object is created after ending the logical incarnation. Doesn't wrap past the maximum value — fails with `GenerationExhausted` instead. |

<a id="owner"></a>
### Owner

The MeshNode that currently actually executes an Actor or Spot and manages its
application queue. The application doesn't directly specify the owner — the
framework finds it through the Location Store.

| Item | Content |
|---|---|
| Shape | The current MeshNode role that authority points to |
| .NET notation | No independent application type. In the provider contract it's expressed with `OwnerId`, `ZLinkLocationOwnerToken`, and an owner node descriptor. |
| Public composition | Verified against authority together with owner identity, owner lease generation, MeshNode RID, and lifecycle generation. |
| Lifetime | Current until an authority owner transition. Replaced by a new owner generation when relocation or takeover succeeds. |

<a id="authority"></a>
### Authority

The reference information that determines which node an Actor or Spot currently
exists on, and which node is currently the owner. The Location Store manages this
information, so different nodes can't consider different owners "the current owner"
at the same time.

Authority isn't just a simple endpoint or send path. It records object identity and
membership, `ObjectGeneration`, `AuthorityOwnerGeneration`, `StoreVersion`, and the
exact owner lease together. The framework checks these values to distinguish:

- a new object re-created under the same Spot ID versus the previous object
- the current owner versus a previous owner
- the current Location Store record versus a stale record from before a change

So a lifecycle change targeting a control message a previous owner sent late, or a
previous object generation, isn't applied to the current object. A regular
Actor/Spot message uses authority's logical ID and the current Ready owner, and
doesn't restrict the handler target by `ObjectGeneration`.

| Item | Content |
|---|---|
| Shape | Composite durable record |
| .NET notation | `ZLinkAuthorityKey` and `ZLinkAuthoritySnapshot` |
| Public composition | Includes object identity, current owner and owner lease, lifecycle state, `ObjectGeneration`, `AuthorityOwnerGeneration`, `StoreVersion`, membership, and placement allocation. |
| Creation/management | Created/changed by the Location Store provider through reservation and compare-exchange transactions. |
| Lifetime | Kept until an explicit fenced delete; not deleted by TTL. |

An authority record lookup passes the object's global logical key as the following
type.

```csharp
public readonly record struct ZLinkAuthorityKey(
    string Value); // provider key corresponding to the object kind and global logical key
```

### Compare-And-Set

A conditional change in the Store that only changes the value if the version received
on read is unchanged. If another request already changed the value first, the change
is rejected as a conflict instead. The framework uses this to prevent two concurrent
requests from changing the same Actor/Spot's owner or membership differently. This
document abbreviates it as CAS.

When CAS targets multiple records, the condition check and every change are handled
in a single Store request. If even one condition differs, no record is changed.

`ZLinkAuthoritySnapshot`'s public fields are as follows.

```csharp
public sealed record ZLinkAuthoritySnapshot(
    string StoreVersion,                         // version compared against the current authority revision
    ReadOnlyMemory<byte> Payload,                // opaque lifecycle payload the framework encoded
    ulong ObjectGeneration,                     // the object incarnation for this key
    ulong AuthorityOwnerGeneration,             // the order the owner changed within the same incarnation
    string OwnerId,                              // current owner identity
    long OwnerLeaseGeneration,                  // current owner process lifecycle fence
    ZLinkPlacementAllocation Allocation,        // Pending or Active capacity allocation
    ZLinkPendingObjectCreation? PendingCreation, // creation info that exists only in the Creating state
    DateTimeOffset StoreNow);                    // the store-relative time the provider returned
```

<a id="ready"></a>
### Ready

The state after Spot creation, initialization, and the Location Store record are all
finished, so it can receive application messages. A Spot direct call generally sends
a message to the owner of a Ready Spot.

| Item | Content |
|---|---|
| Shape | Lifecycle state |
| .NET notation | Expressed as the `Ready` value of a per-feature state enum/snapshot; there's no single common `Ready` type. |
| Public composition | A state where every per-feature serving condition is finished — listener/transport admission, or object creation/initialization. |
| Lifetime | Excluded from the Ready state for new admission once drain, disconnect, relocation, close, or fencing starts. |

<a id="admission-seal"></a>
### Admission Seal

An action that transitions a scope the framework defines so it no longer accepts new
application work. Handlers, replies, and recovery work already accepted can continue
to be processed up to that operation's deadline.

For host shutdown this applies to the whole host. For Actor/Spot relocation it
applies to the single moving target's messages, timers, and not-yet-started
continuations. Admission seal doesn't mean forcibly canceling an already-running
callback.

<a id="owner-route"></a>
### Owner Route

The send path delivering a message from the source runtime to the current owner. When
the owner changes, the framework re-resolves a new owner route.

| Item | Content |
|---|---|
| Shape | Framework-managed routing state |
| .NET notation | No public route type |
| Public composition | Combines the current owner MeshNode identity, transport route, and object generation fence. |
| Lifetime | Usable only while current authority and transport readiness are maintained. |

<a id="owner-fence"></a>
### Owner Fence

A value distinguishing work from the current owner from work a previous owner sent
late. If this value doesn't match the current owner, the receiving node doesn't put
the message on the Spot queue.

| Item | Content |
|---|---|
| Shape | A fence checking several generations and an owner token together |
| .NET notation | No independent public type |
| Public composition | Exactly compares that operation's `ObjectGeneration`, `AuthorityOwnerGeneration`, `OwnerId`, and `OwnerLeaseGeneration` against current authority. Depending on the operation, the expected `StoreVersion` is also checked. |
| Creation/management | Fixed by the framework when it reads authority or receives a reservation. |
| Lifetime | A previous fence becomes stale once the authority owner or owner process lifecycle changes. |

<a id="target-descriptor-fence"></a>
### Target Descriptor Fence

The version of the target's registration information the source checked when
selecting the target. Used to determine whether the target's role, registration type,
or lifecycle information changed after selection.

| Item | Content |
|---|---|
| Shape | A composite fence fixing target descriptor identity and lifecycle |
| .NET notation | A combination of `ZLinkMeshNodeDescriptorKey`, a `ulong` lifecycle generation, and `ZLinkLocationOwnerToken` |
| Public composition | Includes the MeshName/RID descriptor key, target lifecycle generation, and the exact owner lease token. For reservation, capacity delta and descriptor conditions are also verified together. |
| Creation/management | Fixed by the source when selecting a target, and re-verified by the target and Store before reservation. |
| Lifetime | Becomes stale if the descriptor lifecycle or owner lease changes. |

<a id="positive-route-cache"></a>
### Positive Route Cache

Information the source runtime briefly holds — the owner route of a recently
confirmed Ready Spot. Re-queried from the Location Store if no usable cache exists or
it's stale.

| Item | Content |
|---|---|
| Shape | A source-runtime-internal cache entry |
| .NET notation | No public type |
| Public composition | Holds the Ready object key, current owner route, and the generation fence needed for admission. The internal storage format isn't part of the public contract. |
| Creation/management | Created by the framework from a successful Ready authority lookup result and managed by the source runtime. |
| Lifetime | Doesn't exceed `RouteCacheMaxAge`, the owner admission deadline, or the [Message Follow duration](#message-follow-duration) limit. Missing, Creating, and Store failure aren't stored as a positive cache. |

<a id="creation-attempt"></a>
### Creation Attempt

A single creation attempt to build one logical object, running from reservation
through recording the final result. If multiple callers concurrently call
`GetOrCreate` for the same object, the Location Store reservation serializes factory
and callback execution one at a time. Other operations don't share the application
result of an attempt that started earlier.

| Item | Content |
|---|---|
| Shape | Durable state managed by the Location Store |
| Identifying value | [Reservation ID](#reservation-id) |
| Start state | `Reserved` |
| Terminal state | Authority ends in a Ready commit or Creating cleanup. The operation result is recorded in a separate terminal record as `Created`, `Rejected`, or `Failed`. |
| Creation execution | Only the caller that wins the reservation CAS runs the factory and creation callback. |
| Lifetime | The reservation is kept until Ready commit or Creating cleanup. The operation terminal is kept for 5 minutes after the original deadline. |

<a id="reservation-id"></a>
### Reservation ID

An identifier the Location Store uses to distinguish the admitted capacity and
progress record reserved for creation or relocation. Creation IDs and relocation IDs
use separate namespaces. Resending the same request with the same ID returns the
previously issued result. Sending a request with different content under the same ID
is a `Conflict`.

For creation, it can be used to continue the same work after a process restart, or to
cancel exactly that work. For relocation, it's used only to distinguish duplicate
requests within the running source and target processes, and doesn't carry work over
after process termination. It isn't an identifier that merges different operations
into the same application result.

```csharp
public readonly record struct ZLinkCreationReservationId(
    string Value); // opaque value identifying one creation attempt
```

The code above is notation used to explain structure in this document. The actual
public type's name and encoding follow each language's interface contract.

<a id="creation-terminal-result"></a>
### Creation Terminal Result

The final result indicating a creation attempt won't progress further.

| State | Meaning | Ready authority and capacity |
|---|---|---|
| `Created` | The application approved creation, and the object became Ready. | Creates Ready authority and converts reserved capacity to active capacity. |
| `Rejected` | The application callback ran normally but declined creation. May include an optional application reply. | Doesn't create Ready authority or active capacity; returns reserved capacity. |
| `Aborted` | A normal approve/reject result wasn't produced due to node shutdown, timeout, or a callback exception. Includes a typed creation failure. | Doesn't create Ready authority or active capacity; returns reserved capacity. |

`Existing` isn't a creation terminal result. It's the result of looking up an
already-Ready object, so no new creation attempt, reservation, or creation callback
execution occurs.

## 2. Instance Spot Preparation

<a id="instance-intent"></a>
### Instance Intent

The caller's explicit choice that it's fine to prepare a new Instance Spot when the
target Spot doesn't exist. Expressed in .NET by specifying `InstanceSpot(...)` on a
Spot direct call.

| Item | Content |
|---|---|
| Shape | Fluent call option |
| .NET notation | `IZLinkSpotSendCall.InstanceSpot(...)`, `IZLinkSpotRequestCall.InstanceSpot(...)` |
| Public composition | Expressed as whether Instance activation is allowed, plus an optional stable type. |
| Lifetime | Applies only to that single-use call; doesn't carry over to a regular Spot direct or a later call. |

<a id="cold-activation"></a>
### Cold Activation

The process of creating and initializing a new Instance Spot so it can receive its
first message, when the Location Store's authority is `Missing` and the caller
specified Instance intent.

| Item | Content |
|---|---|
| Shape | Framework lifecycle process |
| .NET notation | No independent public type. Observed through the `InstanceSpot(...)` call and `IZLinkInstanceSpot` lifecycle callbacks. |
| Public composition | Made up of the stages: target selection, durable envelope storage, reservation, factory/initialize, inbox first record, Ready commit, and first handler dispatch. |
| Lifetime | Starts from Missing authority and ends in Ready or a fenced terminal failure. |

<a id="activation-envelope"></a>
### Activation Envelope

A delivery unit sent to the target, bundling the first application message together
with the information needed for Spot creation and reply. The target holds this
message and places it on the same Spot queue once, after the Spot becomes Ready.

| Item | Content |
|---|---|
| Shape | Framework-internal composite envelope |
| .NET notation | No public type. The composition table below is contract pseudocode. |
| Creation/management | Created once by the source framework when it starts a call to a Missing Instance Spot, and preserved by the target runtime and the Relocation Store. |
| Lifetime | Kept until the first handler's terminal completion, the replay cursor update, and the release of the recovery pointer. |

| Public composition | Meaning |
|---|---|
| First application message | The payload to process on the same Spot queue after Ready |
| Send/request kind | Distinguishes a one-way message from a request needing a reply |
| Operation identity | Distinguishes whether a retry or a duplicate envelope is the same work |
| Reply correlation | Links the request to its terminal reply |
| Deadline | The final time point applied to the whole activation and request |
| Source identity | Source node RID, lifecycle generation, and an optional source Spot ID |
| Target identity | Global Spot ID, the selected MeshName/stable type, and target descriptor fence |
| Metadata | Whether Command 39's optional metadata is present, plus the immutable metadata frame |

<a id="operation-identity"></a>
### Operation Identity

A value distinguishing whether a retry or duplicate delivery came from the same work.
Used to decide not to process the same first message twice.

| Item | Content |
|---|---|
| Shape | Opaque operation identifier |
| .NET notation | No public type |
| Public composition | A single opaque value. Length and internal encoding aren't part of the public contract. |
| Creation/management | Generated by the framework when starting a terminal-once operation, and kept the same across redirect/recovery. |
| Lifetime | Valid for as long as needed for that operation's terminal completion and duplicate detection. Not generated or interpreted by the application. |

<a id="actor-join-operation-id"></a>
### Actor Join OperationId

A non-zero 128-bit value letting the application distinguish whether an Actor Join
completion callback is redelivering the same result. `RelocationId` — which
identifies the relocation execution itself — the placement reservation ID, and the
bounded aggregate commit ID are each separate internal IDs with different purposes,
and none substitutes for this value.

```csharp
public readonly record struct ZLinkActorJoinOperationId(
    ulong High, // upper 64 bits of the 128-bit ID
    ulong Low); // lower 64 bits of the 128-bit ID
```

| Item | Content |
|---|---|
| Shape | A non-zero 128-bit value made of two `ulong`s |
| .NET notation | `ZLinkActorJoinOperationId` |
| Public composition | `High` and `Low` must be compared together. The application doesn't assign separate meaning to each field. |
| Creation/management | Generated by the framework at Actor Join registration; the same value is passed on every completion retry. |
| Delivery | Included in `Accepted`, `Rejected`, and `Failed` Actor Join completions. For a cross-node `Accepted`, it's also stored in a separate field of the relocation manifest. |
| Lifetime | For same-node outcomes, `Rejected`, and `Failed` before commit, retry is only guaranteed for the current process lifetime. For a cross-node `Accepted`, it's used for durable at-least-once completion for as long as the manifest is kept. |

<a id="deferred-join-barrier"></a>
### Deferred Join Barrier

A process-local queue boundary that runs Actor Join after the current handler ends
normally, and prevents a later-arriving Actor message from overtaking the Join. It's
registered inactive at the moment `Defer()` is called, and doesn't start a target
lookup or Store I/O.

| Item | Content |
|---|---|
| Shape | A framework-internal, handler-scoped queue barrier |
| .NET notation | No public type |
| Public composition | Combines the current Actor identity/`ObjectGeneration`/membership, an immutable join request snapshot, an absolute deadline, and the Actor Join `OperationId`. The internal encoding isn't part of the public contract. |
| Creation/management | Registered by `Defer()` within an open handler registration scope. Activated when the handler ends normally; discarded on exception, cancellation, or reply-encoding failure. |
| Lifetime | Kept from registration until Join terminal and completion ordering finish. If the process terminates before Location commit, this barrier itself isn't replayed. |

<a id="bounded-aggregate-commit"></a>
### Bounded Aggregate Commit

A commit boundary that finalizes several related location-information items together
in one bounded Store transaction, as in cross-node Actor Join. It doesn't expose a
partial state where only the Actor owner changes first and membership or capacity
changes later.

| Item | Content |
|---|---|
| Shape | A bounded multi-record atomic transaction in the Location Store |
| .NET notation | No independent public type |
| Public composition | Verifies and changes Actor authority, source/target membership, capacity, and aggregate generation together. |
| Creation/management | Run once by the framework's relocation coordinator after target staging finishes. |
| Lifetime | A successful commit is the confirmation point of the logical relocation. The same aggregate isn't committed a second time to record callback, relay, and cleanup completion. |

<a id="message-follow"></a>
### Message Follow

The action of forwarding a message that arrives at the previous owner node, on behalf
of the new owner, after an Actor or Spot has relocated to another MeshNode. Its
purpose is to avoid losing a message even when the sender still has the old location
cached — it isn't a redirect that tells the sender the new address and asks it to
resend.

An individual message that arrives late at the previous owner after a relocation
commit is the target of Message Follow. Message Follow isn't kept indefinitely — it's
valid only within the [Message Follow duration](#message-follow-duration); a message
arriving after that period ends is treated as a normal stale-route failure.

This is different from [relocation ingress hold](#relocation-ingress-hold), which the
source holds after sealing during relocation. The hold is temporary storage the
source keeps until commit and then hands to the target queue; Message Follow handles
messages that arrive at the old owner after the commit is done and the owner has
already changed.

| Item | Content |
|---|---|
| Shape | Framework-managed message delivery after an owner transition |
| .NET notation | No public type |
| Public composition | Keeps the new owner's `ActorRef` or Spot location and the Message Follow expiration time. |
| Creation/management | Created by the previous owner's runtime after a relocation commit finishes. |
| Lifetime | Removed once the Message Follow duration ends; a message arriving at the same location afterward is treated as a stale-route failure. |

<a id="message-follow-duration"></a>
### Message Follow Duration

The period for which [Message Follow](#message-follow) is valid. Starts at the
relocation commit and, once it passes, the previous owner no longer forwards.

| Item | Content |
|---|---|
| Shape | A framework-managed duration |
| .NET notation | `ZLinkLocationOptions.MessageFollowDuration` |
| Lifetime | Starts at relocation commit and ends at expiration. The Message Follow entry is removed after expiration. |

<a id="relocation-ingress-hold"></a>
### Relocation Ingress Hold

A size-bounded queue temporarily holding messages that arrive on the previous source
route even after the source Actor's message acceptance is sealed, so they aren't
lost. A message arriving after `Defer()` but before the seal goes not into this hold
but into the Actor queue behind the deferred Join barrier.

| Item | Content |
|---|---|
| Shape | A framework-managed, bounded message hold |
| .NET notation | No public type |
| Public composition | Keeps the message payload, original operation identity, `ObjectGeneration`, and the framework metadata needed for queue ordering. Internal storage format isn't disclosed. |
| Creation/management | The source runtime holds messages arriving after the relocation seal. Regular messaging backpressure and timeout apply once capacity fills. |
| Lifetime | On an abort before commit, restored to the source queue in original order; after a successful commit, relayed to the target queue and then removed. |

<a id="reply-correlation"></a>
### Reply Correlation

Identifying information created when sending a request and kept together on the
request and reply. In the public contract this is a single value called
`correlation_id`, not a combination of several fields.

| Aspect | Contract |
|---|---|
| Value | A single `correlation_id` |
| .NET notation | Observed as `string? CorrelationId` in handler/monitoring context. Can be `null` for a one-way message. |
| Format | A framework-generated opaque ASCII identifier, 1-64 bytes |
| Generator | The MeshNode, ClientServer client, or STREAM runtime that started the request |
| Uniqueness scope | Can't be duplicated among requests concurrently pending within the same lifecycle of the runtime that generated the value. |
| Delivery | The same value is kept on the request and its terminal reply or error. Not generated for a one-way message. |
| Lifetime | Valid until the request terminally completes with a reply, error, timeout, cancellation, or shutdown. |
| Application constraint | Doesn't interpret the value or assemble a new one. Not placed into application metadata as a key. |

`flow_id`, target RID, endpoint, user ID, and payload aren't components of
`correlation_id`. `flow_id` is a separate value for observing a business flow that
chains several messages, and isn't used as the basis for linking a request and reply.

When a reply arrives, the client compares `correlation_id` against the value of the
currently pending request. It's only treated as that request's result if the values
match; if no pending request matches, it's judged a late-arriving reply and
discarded.

This information is kept even when the target has to newly prepare the Spot or
forward the request to the current owner. A downstream request the handler starts
separately uses a value different from the original request's.

The full generation/propagation contract follows [Flow correlation](27-flow-correlation.ko.md).

<a id="deadline"></a>
### Deadline

The final time point by which work must finish. For a request, a single end-to-end
deadline can apply across Spot lookup, cold activation, handler execution, and reply.

| Item | Content |
|---|---|
| Shape | Absolute end-to-end time boundary |
| .NET notation | The caller specifies a `TimeSpan` timeout, while the framework and lifecycle context use a fixed `DateTimeOffset Deadline`. |
| Public composition | A single final time point computed once when the terminal submit starts. Not the sum of per-stage timeouts. |
| Creation/management | Fixed by the source framework from the caller's timeout and the current time. |
| Lifetime | Shared by resolve, reservation, factory, Ready barrier, handler, and reply; discarded after terminal completion. |

<a id="factory"></a>
### Factory

Application-provided code that creates a Spot instance matching a registered stable
type. Even if multiple targets try to create it concurrently, only the target that
first secures creation authority runs the factory.

| Item | Content |
|---|---|
| Shape | Application-provided construction capability |
| .NET notation | For Spot, `AddSpotFactory<TSpot>`/`AddInstanceSpotFactory<TSpot>`; for Actor, `IZLinkActorFactory<TActor>` |
| Public composition | Combines the stable type, per-object-kind factory option, relocation policy, and concrete instance type in the registration. |
| Lifetime | Kept for the duration of the Object Server registration. May run at-least-once during a creation attempt, so it must be retry-safe. |

<a id="activation-barrier"></a>
### Activation Barrier

A boundary preventing the first application message from being delivered to the
handler before Spot initialization and confirmation of the durable activation
inbox's first record finish. The framework opens this boundary after confirming the
`Ready` authority — which keeps the recovery root and replay cursor — and restoring
the first record to the head of the queue.

| Item | Content |
|---|---|
| Shape | Framework-internal admission barrier |
| .NET notation | No public type |
| Public composition | Jointly checks initialize completion, the durable inbox first record, Ready authority, and the local queue-head restore condition. |
| Lifetime | Kept from the start of cold activation until every condition is met and first-handler admission opens. |

<a id="durable-activation-inbox"></a>
### Durable Activation Inbox

A record storing an Instance Spot cold activation's first application message so it
can be restored even after a process restart. The framework only publishes the
Spot's `Ready` authority after confirming this message as the first record.

| Item | Content |
|---|---|
| Shape | Durable ordered record sequence |
| .NET notation | No public type |
| Public composition | Preserves the inbox sequence, the complete activation envelope, and processing-completion state. The provider doesn't interpret the envelope payload. |
| Creation/management | The target runtime confirms the first record in the Relocation Store and durably records handler completion. |
| Lifetime | Created before Ready; kept as recovery evidence until first-handler completion, the replay cursor update, and the release of the recovery pointer. |

<a id="replay-cursor"></a>
### Replay Cursor

The last position durably recorded as processed in the durable activation inbox. The
framework updates the cursor to that inbox sequence after recording the first
handler's completion.

| Item | Content |
|---|---|
| Shape | Monotonic inbox position |
| .NET notation | No public type |
| Public composition | A single inbox sequence — the last one whose terminal completion was recorded. The concrete encoding isn't part of the public contract. |
| Creation/management | Updated by the target runtime after durably recording the handler's terminal completion. |
| Lifetime | Included in the Ready authority's recovery pointer; kept until the pointer is released. Never rolled back to an earlier position. |

<a id="activation-recovery-pointer"></a>
### Activation Recovery Pointer

Information keeping the `Ready` authority pointed at the recovery root and replay
cursor to read when recovering a cold activation. This pointer isn't removed before
the first handler's completion and the cursor update finish.

| Item | Content |
|---|---|
| Shape | A composite pointer of a recovery root reference and a cursor |
| .NET notation | No public type. Included in authority's opaque framework payload. |
| Public composition | Points jointly at an immutable activation recovery root reference and the current replay cursor. |
| Creation/management | Recorded into the authority payload by the Ready commit, and removed by an expected-version `Preserve` CAS. |
| Lifetime | Exists only for a Ready Instance's cold activation. Can't exist on Creating/Closing/Relocating authority or on Actor/Entry/User Spot. |

<a id="recovery-receipt"></a>
### Recovery Receipt

Information confirming the link between the activation recovery root stored in the
Relocation Store and the Pending creation authority. The Location Store records this
together with the creation reservation and returns it on an exact read.

| Item | Content |
|---|---|
| Shape | Immutable content verification record |
| .NET notation | No dedicated public type. On the provider surface it's expressed as a content reference, a `ReadOnlyMemory<byte>` SHA-256, and encoded size. |
| Public composition | Includes the recovery root reference, SHA-256 hash, and encoded byte size. |
| Creation/management | The target stores the root first, then atomically links it to the Location Store reservation. |
| Lifetime | Kept together with the Pending creation authority until Ready commit or fenced-failure cleanup. |

<a id="reservation-fence"></a>
### Reservation Fence

An identifying value the provider issues so only a specific creation reservation can
continue or abort. Prevents a late-arriving commit or abort from a previous
reservation from changing the current creation.

| Item | Content |
|---|---|
| Shape | A provider-issued, composite reservation record |
| .NET notation | `ZLinkObjectReservation` |
| Creation/management | Issued by the Location Store provider on a successful `Reserve`. Commit and Abort exactly compare the same fence. |
| Lifetime | Closes when the corresponding Creating authority commits to Ready or is cleaned up via exact abort. |

```csharp
public sealed record ZLinkObjectReservation(
    ZLinkAuthorityKey Key,                  // authority key for the object kind and global logical key
    string StoreVersion,                    // the Creating authority version Reserve created
    ulong ObjectGeneration,                 // the new object incarnation
    ulong AuthorityOwnerGeneration,         // the initial authority owner generation
    string ReservationVersion,              // the fence letting only this reservation commit/abort
    ZLinkMeshNodeDescriptorKey TargetDescriptor, // the identity of the selected target MeshNode
    ulong TargetNodeLifecycleGeneration,    // target descriptor lifecycle fence
    ZLinkLocationOwnerToken TargetOwner);   // target host owner lease fence
```

## 3. Message Calls And Async Execution

<a id="spot-direct"></a>
### Spot Direct

A way of delivering a send or request to a Spot by specifying a single global Spot
ID. The application doesn't specify an owner RID or endpoint.

| Item | Content |
|---|---|
| Shape | ID-addressed messaging surface |
| .NET notation | `IZLinkSpotClient.SendToSpot<T>()`, `RequestToSpot<T>()`; the resulting call is `IZLinkSpotSendCall`, `IZLinkSpotRequestCall` |
| Public composition | Builds the call from a global Spot ID, typed payload, and optional metadata/timeout/Instance intent. |
| Lifetime | A single-use fluent call; can't be reused after terminal submit. |

<a id="spot-turn"></a>
### Spot Turn

The unit in which one Spot callback occupies an execution gate on the application
queue and runs. Two turns never run at the same time on the same execution gate. A
`SpotWide` User Spot and Instance Spot use one single gate for the whole Spot. Entry
Spot separates a Spot lane from a per-Actor lane. A `PerActor` User Spot separates a
Spot lane, a per-Actor lane, and a per-timer lane, so turns on different gates can
run concurrently.

| Item | Content |
|---|---|
| Shape | Serialized callback execution unit |
| .NET notation | No independent public type. Provided as the execution context of a Spot handler or lifecycle callback. |
| Public composition | Made of one callback, the application queue it entered, and ownership of the execution gate that queue uses. |
| Lifetime | Kept from the callback's start to its completion. In a `SpotWide` User Spot or Instance Spot, `Yield` can return the shared Spot turn first. |

<a id="async-yield"></a>
### Async And Yield

- `Async` holds the current Spot turn while waiting.
- `Yield` returns the shared turn of a `SpotWide` User Spot or Instance Spot so the
  next queue item can run, and resumes execution on a new turn of the same Spot once
  the awaited result is settled. Not usable in other execution contexts.

| Item | Content |
|---|---|
| Shape | Two kinds of request-call terminators |
| .NET notation | `ValueTask<TReply> Async<TReply>(...)`, `ValueTask<TReply> Yield<TReply>(...)` |
| Public composition | Both return the same request result. `Yield` is valid only for `SpotWide` User Spot and Instance Spot, and is provided only for Channel/Spot/Actor requests and CPU/I/O worker calls. |
| Lifetime | Only one terminal method can run per fluent request call. |

Actor/Spot create/get-or-create provide a limited `Yield` that returns the same
result as a request. Actor join, send, publish, timer registration, close, and
destroy don't provide `Yield`. When a `SpotWide` member Actor yields, it only returns
the shared User Spot gate; the [Actor queue claim](#actor-queue-claim) is kept until
the current job finishes.

<a id="submitted"></a>
### One-Way Normal Completion

Normal completion of a one-way call means that source-local outbound admission
accepted the operation. It doesn't return a public status or result value, and
doesn't confirm target handler execution or remote queue acceptance.

<a id="backpressure"></a>
### Backpressure

Flow control that limits send rate via an upper bound on the send queue. A node keeps
a per-peer send queue, and when the bytes occupied by messages the peer hasn't yet
picked up reach that queue's high-water mark, new submissions to that peer are
locked. The high-water mark counts bytes the queue holds, not message count. The
limit is a value **inside the local process**, not a signal the remote sends. Still,
if the remote is slow, the connection's flow control lowers the send rate, so the
send queue doesn't drain — meaning remote delay is conveyed as send-side waiting
rather than as a separate signal.

This wait is why [one-way submit](05-async-execution-policy.en.md#13-one-way-submit)
is async. If capacity is insufficient, the framework waits up to the per-family send
timeout, then submits exactly once; if no room opens up within that window, it
completes with [DeadlineExceeded](#deadlineexceeded). The internal state at this
point is called [Backpressured](#backpressured) and isn't exposed as a public
terminal result.

<a id="backpressured"></a>
### Backpressured

The internal state where the send path or queue's capacity is temporarily
insufficient. Not a public terminal result — the framework waits for capacity up to
the per-family send timeout. Once Logical Multicast has started, per-target capacity
shortfalls aren't aggregated into public results or publish-only monitoring.

<a id="application-hwm"></a>
### Application HWM

A host-wide value limiting the total bytes of payload the framework has already
received but the application handler hasn't finished processing yet. Counts both the
payload waiting in the queue and the payload currently being handled. Once this total
reaches the cap, the framework stops accepting only new application messages. It
keeps processing already-received jobs plus separate Completion-connection request
replies, bounded framework service control, and Core's send-ready callback handling,
so messages aren't dropped — instead
[backpressure](#backpressure) is conveyed to the sender.

`HWM` is short for high-water mark. Where this document uses Application HWM without
a qualifying scope, it means the framework's host-wide limit, distinct from a Core
socket's per-connection HWM.

<a id="timed-out"></a>
### DeadlineExceeded

A framework exception raised when the completion condition for an operation isn't
met by its allowed deadline. The completion condition differs per operation — for
example, a one-way send waits for the source queue to accept the message; object
creation waits until `Ready` or a creation-failure result is confirmed.

Not a public submit status, and distinct from a state where a request handler simply
hasn't returned an application reply yet.

<a id="target-not-found"></a>
### TargetNotFound

The error category an operation family raises when a matching logical target can't
be found or newly prepared. The public framework error kind is `NotFound`.

<a id="route-not-connected"></a>
### RouteNotConnected

An internal transport state raised when the logical target is confirmed but no send
path is currently usable. The public framework error kind is `Unavailable`.

<a id="shutdown"></a>
### Shutdown

A state where the runtime is proceeding with shutdown and can't accept new operation
admission. A new one-way call completes with a `ShuttingDown` exception. The runtime
termination reason and outcome are owned by a separate lifecycle result.

## 4. Channel And Logical Multicast

<a id="channelname"></a>
### ChannelName

A name identifying the Channel scope a message is sent to. In Logical Multicast, it
decides which RouteMesh participating nodes are considered remote target candidates.

| Item | Content |
|---|---|
| Shape | Logical Channel name |
| .NET notation | `string` |
| Public composition | A single string. Doesn't include MeshName, socket, or endpoint. |
| Creation/management | Specified by the application when registering topology and handlers. |
| Lifetime | Kept as a process-local registration key. The same name can't be registered twice for different physical topologies. |

<a id="topic"></a>
### Topic

A value selecting which local Spot subscription should receive a message within the
same ChannelName. Each receiving node checks only its own subscriptions.

| Item | Content |
|---|---|
| Shape | Subscription selector |
| .NET notation | `string` |
| Public composition | A single value passed separately from ChannelName. Doesn't include a Spot ID or remote node list. |
| Creation/management | The application specifies the same value on subscription registration and publish calls. |
| Lifetime | Kept for the duration of the registered subscription. Classic fanout's exact liveness topic can't be used as an application topic. |

<a id="logical-multicast"></a>
### Logical Multicast

A way of delivering one message to multiple Spots in the same Channel, using
ChannelName and topic. The framework sends the message once per remote node, and
each node decides which local Spots actually receive it.

| Item | Content |
|---|---|
| Shape | Multi-target publish surface |
| .NET notation | `IZLinkSpotPublisherClient.Publish<T>()`, `IZLinkPublishCall` |
| Public composition | Takes ChannelName, topic, typed payload, and optional metadata as input, and completes with no return value. |
| Lifetime | Kept while submission is attempted to the targets fixed at the start of one publish transaction. |

<a id="subscription"></a>
### Subscription

Registration information indicating a Spot will receive messages matching a specific
ChannelName, topic, and packet name. The receiving node puts a message into the
queue of a Spot whose registration matches.

| Item | Content |
|---|---|
| Shape | Composite handler registration key |
| .NET notation | No independent public type. Expressed by an `IZLinkSpotHandlerRegistry.AddSubscribe<THandler>(string channelName, string topic)` registration and the handler type. |
| Public composition | A combination of ChannelName, topic, message kind, and packet name. |
| Creation/management | Registered by the application in the Spot's `Configure()`; the framework validates duplicates and Channel membership at startup. |
| Lifetime | Kept for the Spot handler registry's lifecycle; the same exact key can't be registered twice on the same Spot. |

<a id="snapshot"></a>
### Publish Target Snapshot

The remote target list fixed at the start of a publish, together with the matching
local Spot list on the source node. Even if participating nodes change during
publish, the already-started work's snapshot doesn't change.

| Item | Content |
|---|---|
| Shape | The target set fixed at the start of a publish |
| .NET notation | Target identity and count aren't exposed as a public return value. |
| Public composition | The set of positive-weight ready remote MeshNodes, and the matching local Spot set on the source node. |
| Creation/management | Fixed once by the framework when the publish transaction starts. |
| Lifetime | Kept until that publish's target submissions finish; unaffected by mid-flight membership changes. |

<a id="relocation-policy"></a>
### Relocation Policy

The policy — fixed at factory registration — deciding how to handle application
state when an Actor or Spot must keep running on a different node.

| Policy | What's kept on the target |
|---|---|
| `DisableRelocation` | Cross-node relocation isn't allowed. The source owner and application admission are kept. |
| `RecreateOnRelocation` | The application object is re-created by the target factory. Framework queue/timer are kept, but application state isn't restored. `ObjectGeneration` is kept since it's the same logical incarnation. |
| `PreserveStateWith` | Application state at a boundary where the handler ended normally is captured/restored as an opaque byte sequence by the specified relocation adapter. Framework queue/timer are also kept. |

The application can't change the policy per operation, and registration can't be
changed after startup.

<a id="preserve-state-relocation-policy"></a>
### Preserve-State Relocation Policy

A relocation policy that saves application state as bytes when moving an Actor or
Spot to another node, and restores it into a new instance on the target. The
framework-managed queue, unfinished work, and timers move along with it.

The factory configure callback's `PreserveStateWith` also specifies the adapter.

<a id="classic-fanout"></a>
### Classic Fanout

A feature that delivers service events to subscribers using a separate PUB/SUB
socket. Doesn't share a physical connection or subscription state with Spot Logical
Multicast.

| Item | Content |
|---|---|
| Shape | Independent PUB/SUB messaging surface |
| .NET notation | `IZLinkFanoutClient`, `IZLinkFanoutPublishCall`, `IZLinkFanoutHandler<TEvent>` |
| Public composition | Uses a fanout ChannelName, topic, and typed event; has no per-subscriber acknowledgement or replay state. |
| Lifetime | Kept for the publisher/subscriber listener lifecycle and each publish admission. |

## 5. Queue, Control, And Lifetime

### Snapshot

The result of copying runtime state at a specific point in time into a read-only
value. If the actual state changes after the snapshot is taken, the already-returned
value doesn't change. So when using a Snapshot for monitoring or target selection,
don't interpret it as a guarantee that "the state is still the same right now."

| Where used | What the Snapshot represents |
|---|---|
| Monitoring | Node, channel, connection, and capacity state at the moment of the query |
| Publish target | The set of receiving targets fixed when the publish started |
| Metadata | An immutable copy of metadata at the moment it was passed to the handler or send call |

<a id="spot-application-queue"></a>
### Spot Application Queue

The queue that runs Spot direct payloads, matched Logical Multicast payloads, timer
callbacks, and control work that changes Spot state, in order. Actor business
payloads aren't placed on this queue.

| Item | Content |
|---|---|
| Shape | Framework-owned serialized queue |
| .NET notation | No public queue type |
| Public composition | Holds Spot direct, matching publish, timer, and Spot control work items in one order. |
| Lifetime | Kept for the Spot incarnation's duration; sealed/drained/restored per lifecycle rules on close/relocation. |

<a id="object-execution-queue"></a>
### Object Execution Queue

A framework-internal queue holding an Actor's or Spot's application work in
execution order. Before locating the application instance, the framework checks the
message's object identity and generation to find this queue. If a Create is in
progress, the queue can exist even before the application instance does. Relocation
Restore uses the temporary queue described below.

| Item | Content |
|---|---|
| Shape | A framework-owned serialized queue, per object identity and generation |
| .NET notation | No public queue type |
| Public composition | Holds the execution order of lifecycle work like Create, and messages to deliver to the application object once it's ready. |
| Lifetime | Can be created before the object is ready; used for as long as the same incarnation is maintained. If preparation fails or the object is removed, remaining work is ended with a terminal result and the queue is removed once empty. |

During relocation, messages for a target object that isn't ready yet aren't put
directly on this queue. They're first held in the relocation temporary queue defined
below, and moved behind existing work once target preparation finishes.

<a id="relocation-temporary-queue"></a>
### Relocation Temporary Queue

A framework-internal queue that briefly holds messages arriving for an Actor or Spot
while the target runtime is restoring it. Dispatch checks whether a temporary queue
registered for the current relocation exists before locating the Actor or Spot
instance. If one exists, the message goes onto that queue; otherwise it uses the
existing dispatch path.

| Item | Content |
|---|---|
| Shape | A bounded framework queue tied to a `RelocationId`, target attempt, object kind/ID, and `ObjectGeneration` |
| .NET notation | No public queue type |
| Public composition | Preserves target identity, original operation identity, deadline, payload, and reply route. For `SpotWide`, the Spot and member Actors go in the same relocation group, but each record preserves its actual target. |
| Lifetime | Registered when the target accepts a Restore request. Work is moved to the real object queue and this queue removed after commit and any needed callbacks. Discarded without running on an abort before commit. |

The switchover from temporary queue to real object queue is handled atomically.
Messages accepted before the switch stay on the temporary queue; messages accepted
after the switch go straight into the real queue. The framework puts the previously
saved work into the real queue first, then the temporary queue's work after it. The
real queue's application handlers don't run until all of this is finished.

<a id="spot-control-claim"></a>
### Spot Control Claim

Control work that changes state a Spot manages, following an Actor join, leave, or
lifecycle change. Runs in the same queue order as the target Spot's other callbacks.

| Item | Content |
|---|---|
| Shape | Spot queue control work item |
| .NET notation | No independent public type. Expressed as the result of Actor join/leave and lifecycle APIs. |
| Public composition | Has target Spot identity, control kind, and the Actor/lifecycle information to apply. The internal envelope isn't public. |
| Lifetime | Valid until that control callback finishes on the Spot application queue. |

<a id="actor-queue-claim"></a>
### Actor Queue Claim

The right to run the single current job at the head of an Actor queue. Prevents two
jobs on the same Actor from overlapping, or a later job running before an earlier
one.

| Item | Content |
|---|---|
| Shape | Framework-owned Actor queue execution claim |
| .NET notation | No independent public type. Provided as the Actor handler's execution context. |
| Public composition | Combines Actor identity with the current queue-head job. |
| Lifetime | Kept from handler start through completion of the current job, including its continuation. Even if a `SpotWide` member Actor yields, only the User Spot gate is returned — this claim is kept. |

<a id="relocation-mode"></a>
### Relocation Mode

The caller's intent specifying which application version to move a host's stateful
objects to. Use `PlannedMaintenance` for node maintenance that keeps the application
version, and `RollingUpdate` for a deployment that switches to a prepared new
version.

| Item | Content |
|---|---|
| Shape | Closed value `PlannedMaintenance=0`, `RollingUpdate=1` |
| .NET notation | `ZLinkFrameworkRelocationMode` |
| Public composition | `PlannedMaintenance` uses the same effective target version as the source. `RollingUpdate` also specifies an exact `TargetApplicationVersion` greater than the source's. |
| Lifetime | Fixed when a host `Relocate` operation starts; the same mode and effective target version are also recorded in the terminal result. |

The exact version the mode determines is applied first, and then capability, policy,
adapter, capacity, and placement weight are evaluated. A node with a version
different from the requested one isn't a target, even if it's a higher version.

<a id="relocation-unit"></a>
### Relocation Unit

The smallest bundle of Actors or a Spot for which the framework, during host
relocation, blocks new work on the source once, restores it on the target, and then
switches the currently processing node. Different units can move concurrently, in
the order they finish preparing. If a unit includes both Actors and a Spot together,
the node handling the Actors and the Spot they belong to are changed together per
that unit's contract.

| Item | Content |
|---|---|
| Shape | Either a single Actor, or a Spot bundled with the Actors that must move together. No independent public type. |
| Public composition | One of: a single Entry Spot Actor, a single Actor of a `PerActor` User Spot, a `PerActor` Spot message target, a `SpotWide` User Spot with all of its member Actors, or a single Instance Spot. |
| Creation/management | Built by the framework handling the host `Relocate`, based on the currently processing node and the Spot execution mode. The application doesn't add or remove members of the unit. |
| Delivery | The Relocation Store records the unit's identity, its saved state/queue/timer, and target restore information. Not exposed in application messages. |
| Lifetime | Created when the source blocks new work and starts the move; ends when the target starts processing after the location change, or the move is canceled before the location change. |
| Application authority | The application doesn't directly create or change a unit. Only when `ApplicationSignaled` is chosen under `SpotWide` can it signal a safe turn to start the move. |

<a id="maintenance-wave"></a>
### Maintenance Wave

An application configuration value distinguishing a bundle of hosts that shouldn't
be taken down together in the same maintenance operation. If the source's and
target's maintenance wave are the same, that target is excluded from relocation
candidates.

If the value isn't set, this exclusion rule doesn't apply. The framework compares
the configured string as a whole, case-sensitively.

<a id="drain"></a>
### Drain And Draining

Drain is the process of closing admission for new application work in order to shut
down a host, and cleaning up already-accepted work and infrastructure resources
within a fixed time. While this process is in progress, the state is called
`draining` or "in drain." Relocation — moving stateful objects to another host — is a
separate operation; a host becomes `Relocated` when it succeeds.

Starting a drain doesn't immediately cut existing connections or cancel
already-accepted work right away. Which new work is blocked and how long existing
work continues to be processed depends on the component and the `Shutdown` stage.
`Relocate` keeps processing existing application work until each unit's seal, and
doesn't shut down the host even on success.

On a ClientServer Server, starting drain excludes it from target selection for new
sends and requests and stops accepting new business messages. Already-accepted
handlers and request replies are processed up to their deadline, and then descriptor,
owner lease, and listener are cleaned up. Unlike weight `0`, which only zeroes
selection share while keeping the Server running, drain is a lifecycle procedure that
completes shutdown.

| Item | Content |
|---|---|
| Shape | Lifecycle process and closed state |
| .NET notation | `IZLinkFrameworkRuntime`, `ZLinkFrameworkRuntimeState`, `ZLinkFrameworkRuntimeEvent` |
| Public composition | Uses `RelocateAsync`, which takes a mode and exact target application version; a separate `ShutdownAsync`; and host runtime state and deadline. |
| Lifetime | Proceeds from the start of `Shutdown` to either normal cleanup or force-stop completion, preserving the deadline of already-accepted work. |

<a id="drain-deadline"></a>
### Drain Deadline

The time allowed, after shutdown starts, to finish already-accepted work and
lifecycle cleanup. Once shutdown starts, no new application payload is accepted.

| Item | Content |
|---|---|
| Shape | Absolute lifecycle deadline |
| .NET notation | `DateTimeOffset Deadline`; the drain-start API can also take a duration as `TimeSpan?`. |
| Public composition | A single final time point fixed when drain starts. |
| Creation/management | Computed by the framework from a caller-specified duration or a per-feature default. |
| Lifetime | Shared by already-accepted work and lifecycle cleanup up to this point. Per-feature force-stop/fence rules apply after the deadline. |

<a id="metadata-snapshot"></a>
### Metadata Snapshot

A small key-value payload delivered separately from the business payload. The
framework fixes it as an immutable snapshot before providing it to the handler
context. The actual copy timing and internal storage method aren't part of the
public contract.

| Item | Content |
|---|---|
| Shape | Immutable key-value snapshot |
| .NET notation | `ZLinkMessageMetadata` and `IReadOnlyDictionary<string, string>` |
| Public composition | A map of UTF-8 keys and values. NUL isn't allowed; encoded keys/values plus structural overhead total at most 1024 bytes. |
| Creation/management | The application sets it on the outbound builder, and the framework fixes it as an immutable snapshot at submit. For the same key, the last value set applies. |
| Lifetime | Valid until the handler turn ends. The application must copy it to keep it; request metadata isn't auto-copied to a reply. |

`ZLinkMessageMetadata`'s public surface is as follows.

```csharp
public sealed class ZLinkMessageMetadata
{
    // builds an immutable metadata snapshot from the given key-values.
    public ZLinkMessageMetadata(
        IReadOnlyDictionary<string, string> values);

    public static ZLinkMessageMetadata Empty { get; } // a snapshot with no values

    // exposes the whole key-value set as an unmodifiable view.
    public IReadOnlyDictionary<string, string> Values { get; }

    public string? Find(string key); // returns null if the key isn't present.
}
```

## 6. RouteMesh And Channel Topology

<a id="membership"></a>
### Membership

Registration information stating that a node or Server participates in a specific
Mesh or Channel. ChannelName Server membership includes the weight used for handler
and target selection.

| Item | Content |
|---|---|
| Shape | Composite topology registration |
| .NET notation | No independent public type. Expressed via builder registration and monitoring's `ZLinkMeshChannelSnapshot`. |
| Public composition | Includes MeshName, ChannelName, Client/Server role, and — when Server — the weight/handler namespace. |
| Creation/management | Registered by the application in the startup builder; the framework reflects it into the descriptor and process-local channel index. |
| Lifetime | Kept for the lifecycle of that MeshNode or ClientServer registration. Drain and weight changes only change selectability. |

<a id="channel-client-server-role"></a>
### Channel Client And Server Role

- Client role only registers the send path to start Channel calls.
- Server role registers the send path plus remote target membership, and provides
  handlers and weight.

Server role also includes Client's send functionality.

| Item | Content |
|---|---|
| Shape | Closed registration role |
| .NET notation | ClientServer monitoring uses `ZLinkClientServerRole`; the RouteMesh builder expresses it via Client/Server registration methods. |
| Public composition | Client registers only the send path; Server registers the send path plus target membership, handler namespace, and weight. |
| Creation/management | The application registers at most once per role for the same ChannelName. |
| Lifetime | Fixed during host startup configuration. Weight `0` or drain doesn't turn a Server into a Client role. |

<a id="weight"></a>
### Weight

A relative share, `0..10000`, deciding how often new work is assigned when choosing
among several ready targets. It doesn't mean the number of concurrent jobs a target
can handle, or its physical performance. For example, if two otherwise-equal targets
have weight `100` and `50`, repeated selection assigns twice the share to the target
with `100`.

`0` excludes a target from new select-one and Logical Multicast remote target
selection, but doesn't cancel already-submitted work. Setting weight to `0` alone
doesn't put the target into drain state or start a shutdown procedure. Raising weight
back up while running lets a target satisfying other conditions become a candidate
again.

| Item | Content |
|---|---|
| Shape | Relative selection weight |
| .NET notation | `int` |
| Public composition | A single integer in `0..10000`; the default is `100`. A value outside the range is a configuration error, both at startup and at runtime change. |
| Creation/management | Specified by the application at Server registration and changed via an allowed runtime API. Descriptor revision orders the changes. |
| Lifetime | Kept for the Server's lifecycle. `0` only excludes it from new selection — it doesn't remove the role, connection, or already-submitted work. |

Node placement, RouteMesh Channel Server, and ClientServer Server use the same range
and default. Weighted selection computes the candidate weight sum using at least a
64-bit integer. Logical Multicast includes an eligible remote member exactly once,
regardless of the magnitude of its positive weight.

<a id="full-mesh"></a>
### Full Mesh

The topology that directly connects MeshNode pairs that need to exchange messages
within the same MeshName. With `N` nodes, each node manages at most `N-1` peer
connections. If both nodes are Object Client and neither has RouteMesh Channel
Server membership, they aren't connected, so the actual connection count can be
lower than this cap.

| Item | Content |
|---|---|
| Shape | RouteMesh connection topology |
| .NET notation | Configured via `IZLinkMeshNodeBuilder.PeerConnections` and observed via `ZLinkMeshNodeSnapshot.Peers`. |
| Public composition | The set of direct peer connections for pairs that need one, among the MeshNodes sharing a MeshName. Only excludes pairs where both sides are Object Client with no RouteMesh Channel Server membership. |
| Lifetime | Reconciled based on MeshNode join/leave and readiness; each connection has its own independent lifecycle. |

<a id="peer-admission"></a>
### Peer Admission

The process of checking a connected remote node's MeshName, RID, lifecycle,
descriptor, object role, and security identity to decide whether to accept it as a
ready peer connection. For a manual connection, if both sides are Object Client with
no RouteMesh Channel Server membership, a terminal admission result recording that
the connection isn't needed is recorded, and the socket is closed before becoming
ready.

| Item | Content |
|---|---|
| Shape | Transport validation process |
| .NET notation | No independent public result type. Peer state is observed via `ZLinkPeerStatus.State`, distinguishing connection failure `NotConnected` from normal omission `NotRequired`. |
| Public composition | Made of MeshName, RID, lifecycle generation, object role, descriptor condition, protocol capability, and security identity verification. |
| Lifetime | Performed for every new connection. For the same manual endpoint and configuration generation, a result that ended as an Object Client pair with no Server membership isn't retried until the connection configuration changes. |

`NotRequired` means both nodes are Object Client and neither has RouteMesh Channel
Server membership, so no connection is needed. This peer is still shown in public
monitoring but excluded from ready/liveness/health failure aggregation.
`NotConnected` means a connection is needed but no ready connection exists, and it's
reflected in failure aggregation.

<a id="lifecycle-generation"></a>
### Lifecycle Generation

A value distinguishing a previous run from the current run when the same logical
server or listener restarts. Used so a late frame or reply from a previous
generation isn't applied to the current connection.

| Item | Content |
|---|---|
| Shape | A non-zero opaque equality token. Execution order isn't judged by numeric magnitude. |
| .NET notation | `ulong LifecycleGeneration` |
| Public composition | A single non-zero generation value. A restarted run at the same endpoint uses a new value different from the previous one. |
| Creation/management | The framework determines the value to use for each new listener/server lifecycle. |
| Lifetime | Kept until that run ends. The remote compares the descriptor's and transport admission's values exactly. |

<a id="descriptor"></a>
### Descriptor

Registration information a remote runtime publishes so its endpoint, identity,
Channel membership, weight, and state can be discovered. RouteMesh, ClientServer, and
fanout each use a different kind of descriptor. `Descriptor` is a generic term for
this registration information; the actual documents specify which topology's
information it is — `MeshNode descriptor`, `ClientServer Server descriptor`, `fanout
publisher descriptor`.

| Item | Content |
|---|---|
| Shape | The parent concept of a discovery record |
| .NET notation | No common base type; split into `ZLinkMeshNodeDescriptor`, `ZLinkClientServerServerDescriptor`, `ZLinkFanoutPublisherDescriptor`. |
| Public composition | Commonly has identity, lifecycle, advertised endpoint, state, and exact owner lease; each per-topology field is defined by its own descriptor entry. |
| Lifetime | An ephemeral record tied to a host owner lease. |

<a id="meshnode-descriptor"></a>
### MeshNode Descriptor

A RouteMesh-specific registration a MeshNode publishes to the Location Store in
automatic discovery, to tell other nodes its identity and connection information.
Not a general object description — it's the framework contract a remote MeshNode
uses to find and verify peer connection candidates.

A MeshNode descriptor includes the following information.

- MeshName and RID
- Lifecycle generation and descriptor revision
- The actual advertised ROUTER endpoint to connect to
- The Server ChannelName set and per-Channel weight
- Object role, one of `None`, `Client`, `Server`
- The security identity used to verify the connecting peer
- Protocol version and required capabilities
- If this is an Object Server, the exact Entry Spot ID issued in the same lifecycle

| Item | Content |
|---|---|
| Shape | Composite discovery record |
| .NET notation | `ZLinkMeshNodeDescriptor`; key is `ZLinkMeshNodeDescriptorKey` |
| Creation/management | The MeshNode runtime publishes this to the Location Store after listener bind and admission info are confirmed, and refreshes it via the owner lease. |
| Lifetime | An ephemeral record tied to the host owner lease. A new lifecycle publishes a descriptor with a new generation. |

```csharp
public sealed record ZLinkMeshNodeDescriptor(
    string MeshName,                              // RouteMesh namespace
    RoutingId Rid,                               // MeshNode transport identity
    ulong LifecycleGeneration,                   // the current MeshNode run
    ulong DescriptorRevision,                    // change order within the same lifecycle
    string Endpoint,                             // the actual advertised ROUTER endpoint
    string? EntrySpotId,                        // exact Entry Spot ID for the Object Server lifecycle
    IReadOnlyDictionary<string, int> ChannelWeights, // per-Server-Channel selection weight
    string SecurityIdentity,                     // transport peer admission identity
    string OwnerId,                              // the host owner that published the descriptor
    long LeaseGeneration,                        // host process lifecycle fence
    DateTimeOffset UpdatedAt)                    // when the Store recorded the update
{
    public long ApplicationVersion { get; init; } // application deployment sequence number

    // object kind/stable type/policy/placement capability
    public IReadOnlyList<ZLinkObjectCapability> ObjectCapabilities { get; init; }
        = Array.Empty<ZLinkObjectCapability>();

    public string? MaintenanceWave { get; init; } // optional maintenance wave stable ID
    public ZLinkFrameworkRuntimeState State { get; init; } // runtime state
    public ZLinkMeshNodeObjectRole ObjectRole { get; init; } // Object Client/Server role
    public int PlacementWeight { get; init; } = 100; // object placement selection weight

}

public readonly record struct ZLinkMeshNodeDescriptorKey(
    string MeshName, // RouteMesh namespace
    RoutingId Rid);  // MeshNode transport identity
```

A descriptor's capacity information is a projection that separately tracks active/
reserved count and configured limit for Actors overall, User/Instance Spots overall,
and per Spot stable type. It doesn't use a single combined active-object cap of
`10,000`. Limit `0` means no limit, and Entry Spot isn't counted in Spot capacity.
Descriptor values are only used to quickly filter candidates — actual slot
acquisition is confirmed by the Location Store's atomic reservation.

A remote MeshNode checks the endpoint and Object role from this registration
information. If both descriptors' Object role is `Client`, an automatic connection
intent isn't created. For every other pair, the actual transport handshake must also
re-verify that MeshName, RID, lifecycle generation, Object role, and security
identity match the registration information, before completing peer admission.

ClientServer Server, fanout publisher, and Spot/Actor location aren't recorded in the
MeshNode descriptor. Each feature uses its own descriptor or location record.

<a id="clientserver-server-descriptor"></a>
### ClientServer Server Descriptor

Registration information a Server publishes to the Location Store in ClientServer
Channel automatic discovery, to tell Clients its identity, connection location, and
selection state. The Server publishes this together with an owner lease.

Includes the following information.

- ChannelName
- Server RID and lifecycle generation
- The actual advertised endpoint to connect to
- Weight and drain state
- Descriptor revision

| Item | Content |
|---|---|
| Shape | Composite discovery record |
| .NET notation | `ZLinkClientServerServerDescriptor`; key is `ZLinkClientServerServerDescriptorKey` |
| Creation/management | The ClientServer Server runtime publishes listener and selection state to the Location Store and refreshes it via the owner lease. |
| Lifetime | An ephemeral record tied to the host owner lease. A listener restart uses a new lifecycle generation. |

```csharp
public sealed record ZLinkClientServerServerDescriptor(
    string ChannelName,                 // the service Channel Clients look up
    RoutingId ServerRid,                // Server identity
    ulong LifecycleGeneration,          // the current Server run
    ulong DescriptorRevision,           // weight/drain-state change order
    string Endpoint,                    // the actual advertised endpoint
    int Weight,                         // relative selection share for new requests/sends
    ZLinkFrameworkRuntimeState State,   // runtime state such as serving/draining
    string SecurityIdentity,            // transport admission identity
    string OwnerId,                     // the host owner that published the descriptor
    long LeaseGeneration,               // host process lifecycle fence
    DateTimeOffset UpdatedAt);           // when the Store recorded the update

public readonly record struct ZLinkClientServerServerDescriptorKey(
    string ChannelName,   // ClientServer Channel
    RoutingId ServerRid); // Server identity
```

The owner lease proves, by renewing on a fixed schedule, that the Server still has
the right to keep using this registration information.

The Client looks up the endpoint from valid registration information for the same
ChannelName. The actual transport connection must also re-verify that Server
identity, lifecycle generation, and security identity match the registration
information before using it as a ready target.

MeshName, RouteMesh membership, and Spot/Actor location aren't recorded in the
ClientServer Server descriptor. ClientServer discovery also doesn't substitute the
MeshNode descriptor for this.

<a id="fanout-publisher-descriptor"></a>
### Fanout Publisher Descriptor

Registration information a publisher publishes to the Location Store in Classic
fanout automatic discovery, to tell subscribers its identity and PUB endpoint.
Includes ChannelName, Publisher RID, lifecycle generation, and advertised endpoint.

| Item | Content |
|---|---|
| Shape | Composite discovery record |
| .NET notation | `ZLinkFanoutPublisherDescriptor`; key is `ZLinkFanoutPublisherDescriptorKey` |
| Creation/management | The publisher runtime publishes this to the Location Store after PUB listener bind, and refreshes it via the owner lease. |
| Lifetime | An ephemeral record tied to the host owner lease. A new generation is used when the publisher lifecycle changes. |

```csharp
public sealed record ZLinkFanoutPublisherDescriptor(
    string ChannelName,                // fanout Channel
    RoutingId PublisherRid,            // publisher identity
    ulong LifecycleGeneration,         // the current publisher run
    ulong DescriptorRevision,          // change order within the same lifecycle
    string Endpoint,                   // the advertised PUB endpoint subscribers connect to
    ZLinkFrameworkRuntimeState State,  // publisher runtime state
    string SecurityIdentity,           // connection admission identity
    string OwnerId,                    // the host owner that published the descriptor
    long LeaseGeneration,              // host process lifecycle fence
    DateTimeOffset UpdatedAt);          // when the Store recorded the update

public readonly record struct ZLinkFanoutPublisherDescriptorKey(
    string ChannelName,      // fanout Channel
    RoutingId PublisherRid); // publisher identity
```

A subscriber only looks up publisher descriptors for the same fanout ChannelName. It
doesn't use a MeshNode or ClientServer Server descriptor as fanout connection
information. It creates a separate SUB socket per publisher endpoint and judges each
connection's ready and liveness independently.

<a id="descriptor-revision"></a>
### Descriptor Revision

A monotonically increasing number, starting at 1, marking the version of mutable
descriptor information — like weight or drain state — within the same lifecycle.
Current state is never rolled back to a lower revision.

| Item | Content |
|---|---|
| Shape | Monotonic revision |
| .NET notation | `ulong DescriptorRevision` |
| Public composition | A single integer of at least 1. Compared together with lifecycle generation. |
| Creation/management | Incremented by the descriptor owner whenever it changes public state within the same lifecycle. |
| Lifetime | Ordering is compared only within that lifecycle. A new lifecycle's revision isn't directly compared to a previous lifecycle's value. |

<a id="automatic-discovery"></a>
### Automatic Discovery

A way of finding a remote endpoint and identity by querying descriptors published to
the Location Store. After finding the descriptor, identity and lifecycle generation
must also be verified on the actual transport connection before it's used as a ready
target.

| Item | Content |
|---|---|
| Shape | Descriptor-based discovery process |
| .NET notation | Used via `IZLinkLocationStore` registration and topology builder configuration that omits an endpoint. |
| Public composition | Made of the stages: descriptor query, excluding Object Client pairs with no Server membership, desired-set reconcile, transport connect, and identity/lifecycle admission. |
| Lifetime | Repeated by the host runtime for the duration of store polling and connection lifecycle. |

<a id="manual-discovery"></a>
### Manual Endpoint

A way of directly registering a remote endpoint through application configuration.
The endpoint alone isn't trusted — the actual connection's identity and
configuration condition are re-verified.

| Item | Content |
|---|---|
| Shape | Application-provided endpoint configuration |
| .NET notation | Specified as a `string` on each topology builder's `Listen(string)`, manual peer, or subscriber endpoint method. |
| Public composition | Made of the remote endpoint and the expected-identity condition the topology requires. A pair where both sides are Object Client with no RouteMesh Channel Server membership is excluded from handshake admission before becoming ready. |
| Lifetime | Fixed during host startup configuration. Reconnecting a pair that needs a connection re-runs transport admission. A pair excluded under the same endpoint and configuration generation isn't reconnected until the configuration changes. |

<a id="ready-target"></a>
### Ready Target

A target where the listener, transport connection, identity check, and required
handler registration are all finished, so it can receive new messages.

| Item | Content |
|---|---|
| Shape | Selectable runtime target state |
| .NET notation | `ZLinkMeshPeerSnapshot.Ready`, `ZLinkClientServerServerSnapshot.Ready`, and per-feature state enums |
| Public composition | Transport is ready, identity/lifecycle checks passed, and required handler/role conditions are met. A select-one candidate must also satisfy positive weight and non-draining. |
| Lifetime | Immediately excluded from new target selection once any condition closes. |

<a id="max-message-size"></a>
### MaxMessageSize

The byte cap on the complete transport message a listener can receive. A message
exceeding the cap doesn't have any partial payload delivered to the handler. A regular
application listener uses the value owned by its socket options. A StreamNode has a
separate rule for its Core STREAM inbound path.

For a StreamNode, `MaxMessageSize` checks a complete message received from client to
server. Its size is the sum of header bytes and payload bytes, excluding the 6-byte
prefix. The default is `64 KiB` (`65,536` bytes). `0` adds no separate Framework cap and
maps to Core `ZLINK_OPT_MAXMSGSIZE = -1`. A positive value is finite, and a negative value
is a startup configuration error. This cap isn't applied to a message sent from server
to client. An over-limit message isn't partly delivered to the session handler; the
server records `EMSGSIZE` and closes the connection. A raw client receives no separate
error code and observes only the connection closing.

| Item | Content |
|---|---|
| Shape | Byte-size configuration limit |
| .NET notation | `long MaxMessageSize` for a regular socket, and `ConfigureSocket().MaxMessageSize` for a StreamNode. |
| Public composition | Applies to the complete message received by the socket owner. A regular socket's `0` uses its binding or transport default; a StreamNode's `0` maps to Core `-1`. |
| Creation/management | The application sets it in the listener or StreamNode socket options before startup. |
| Lifetime | Fixed for the listener lifecycle; the StreamNode value is fixed after startup. Framework doesn't add the StreamNode-specific cap to ClientServer listeners or RouteMesh SS transport; ClientServer keeps its regular application-listener rule. |

## 7. Channel Messaging

<a id="node-direct"></a>
### Node Direct

A way for the caller to specify both MeshName and a target RID to send a message to
a specific MeshNode. The framework doesn't substitute a different node for the
specified RID. Object Client can't register an application Node direct handler, so
it isn't a Node direct target.

| Item | Content |
|---|---|
| Shape | Explicit node-addressed messaging surface |
| .NET notation | `IZLinkRouteClient`'s node-direct send/request calls |
| Public composition | Built from MeshName, a target `RoutingId` that isn't Object Client, typed payload, and optional metadata/timeout. |
| Lifetime | A single-use call; doesn't switch to a different node if no route exists for the specified RID. |

<a id="select-one"></a>
### Select-One

A way of choosing one Server, among several ready Servers participating in a
ChannelName, to receive the current call. Reflects weight, ready, and drain state
together, and submits the message to the selected target within the same operation.

| Item | Content |
|---|---|
| Shape | Atomic target selection and submit operation |
| .NET notation | Internal behavior of the Channel send/request call builder; doesn't return the selected Server identity as an intermediate result. |
| Public composition | Made of the process-local ChannelName route, the eligible ready target snapshot, and weight/drain filters. |
| Lifetime | Ends once a single target is selected and the message is submitted within one submit operation. |

<a id="handler-namespace"></a>
### Handler Namespace

A scope distinguishing which handler registration scope to look up the same packet
name in. Node direct and ChannelName handlers use different namespaces.

| Item | Content |
|---|---|
| Shape | Composite handler lookup scope |
| .NET notation | No independent public type. Distinguished by handler registry and context type. |
| Public composition | Node direct uses MeshName, message kind, and packet name; Channel handlers use ChannelName, message kind, and packet name. |
| Creation/management | The framework builds an index from startup handler registration. |
| Lifetime | Kept for the host handler registry's lifecycle; the same exact key can't be registered twice within the same namespace. |

<a id="message-kind"></a>
### Message Kind

A value distinguishing which processing style a message uses — send, request, or
publish. Used together with ChannelName or MeshName and packet name when looking up
a handler.

| Item | Content |
|---|---|
| Shape | Closed message category |
| .NET notation | Distinguished by handler type; monitoring uses `ZLinkDispatchMessageKind`. |
| Public composition | A single category — Send, Request, Publish — each with a different completion/reply style. |
| Creation/management | Determined by the framework from the send/request/publish surface that was called. |
| Lifetime | Unchanged for the duration of the message envelope and handler lookup. |

<a id="packet-name"></a>
### Packet Name

The message name selecting a typed handler within the same handler namespace. A
handler with the same message kind and packet name can't be registered twice in the
same namespace.

| Item | Content |
|---|---|
| Shape | Typed handler selector |
| .NET notation | The default name is a `string` the framework determines from the message type. In the Stream Connector, `IZlinkStreamSendCall.PacketName(string)` and `IZlinkStreamRequestCall.PacketName(string)` can specify a per-call name. |
| Public composition | A single name compared within a handler namespace and message kind. Doesn't include payload bytes or correlation. |
| Creation/management | Fixed by the framework at typed message registration. The application can specify a per-call name when the given public contract allows an override. Codec doesn't determine packet name. |
| Lifetime | Must stay stable for that message and handler registration. |

<a id="liveness-beacon"></a>
### Liveness And Liveness Beacon

Liveness checking is the action of confirming whether signals from a connected peer
keep arriving within a fixed time, to judge whether a connection can be kept in a
Ready state. It isn't a feature that confirms application-message handler execution
or business processing success.

Liveness beacon is an internal message the runtime periodically sends so a
one-directional connection's liveness can be checked. Classic fanout's
connection-status-checking topic and beacon aren't exposed to application publish or
handlers.

| Item | Content |
|---|---|
| Shape | An exact two-frame internal multipart record |
| .NET notation | No public type |
| Public composition | Exactly two frames: the topic frame `01 5A 4C 46 31` and the payload frame `5A 46 01 01`. |
| Creation/management | Sent by the fanout publisher runtime every 5 seconds, independent of application publish. |
| Lifetime | The subscriber processes only the exact record as a liveness signal. If the topic matches but the payload or frame count differs, it's a protocol error and isn't delivered to an application handler. |

## 8. ClientServer Channel

<a id="clientserver-channel"></a>
### ClientServer Channel

A one-directional service boundary where the Client starts sends/requests and the
Server handles execution and reply. The Server doesn't start new business calls
targeting the Client. One process can register a Client and a Server each once for
the same ChannelName. Monitoring's `client_and_server` is a snapshot expression
meaning both registrations exist together — it isn't a separate builder role.

| Item | Content |
|---|---|
| Shape | Client-initiated service topology |
| .NET notation | `IZLinkClientServerChannelRoleBuilder`, `IZLinkClientServerRuntime`, `ZLinkClientServerChannelSnapshot` |
| Public composition | Made of ChannelName, Client/Server registration, Server descriptor/connection set, handler namespace, and select-one state. |
| Lifetime | Kept for the host registration lifecycle; Client and Server roles are each registered at most once. |

<a id="server-identity"></a>
### Server Identity

A value identifying a specific Server run in a ClientServer connection. Checked
together with lifecycle generation so a stale connection from before a restart isn't
used as a new target.

| Item | Content |
|---|---|
| Shape | Composite identity of Server RID and lifecycle |
| .NET notation | `ZLinkClientServerServerDescriptorKey(ChannelName, ServerRid)` and `ulong LifecycleGeneration` |
| Public composition | Used together: ChannelName, `RoutingId` Server RID, and lifecycle generation. Endpoint isn't part of identity. |
| Creation/management | Fixed by the Server runtime at the start of the listener lifecycle, and provided identically to the descriptor and transport admission. |
| Lifetime | Valid for that Server's lifecycle. A restart uses a new lifecycle generation, even at the same endpoint. |

<a id="reply-token"></a>
### Reply Token

A one-time-use capability the framework provides so a Server request handler can
reply to the current request. Once the final reply is made, it can't be reused.

| Item | Content |
|---|---|
| Shape | One-shot reply capability |
| .NET notation | Doesn't expose an independent token type to the application — expressed as the single reply return of a typed request handler. |
| Public composition | An opaque capability tied to the current request correlation and terminal-once state. The internal handle and route aren't public. |
| Creation/management | Created by the framework at request dispatch, and converted from handler completion into a terminal reply. |
| Lifetime | Closes on the first terminal reply, error, or request cancellation/timeout. Can't be reused once closed. |

<a id="downstream-request"></a>
### Downstream Request

A new request a handler sends to a different RouteMesh, ClientServer Channel, Spot,
or Actor while it's processing the original request. Uses a different correlation
than the original request.

| Item | Content |
|---|---|
| Shape | A handler-originated independent request |
| .NET notation | That target surface's `IZLinkRequestCall`, `IZLinkSpotRequestCall`, or `IZLinkActorRequestCall` |
| Public composition | Made of a new target, typed payload, new reply correlation, and optional metadata/timeout. |
| Lifetime | Has terminal completion independent of the original request; its result doesn't get folded back into the original correlation. |

<a id="owner-lease"></a>
### Owner Lease

Information proving, by renewing on a fixed schedule, that a framework host still
has the right to keep using the current lifecycle's registration information and
object ownership. The same host token fences MeshNode/ClientServer Server/fanout
publisher descriptors, automatic RID claims, Actor/Spot authority, and maintenance
role.

| Item | Content |
|---|---|
| Shape | A composite lease record of an owner token and expiry |
| .NET notation | `ZLinkLocationOwnerToken`, `ZLinkOwnerLeaseReadResult.Found` |
| Creation/management | The Location Store provider issues a generation on claim, and the host renews it on a fixed schedule. |
| Lifetime | Valid until `LeaseExpiresAt`. Stale if the exact owner token differs or the store-relative expiry has passed. |

Owner identity is bundled into the following `ZLinkLocationOwnerToken`.

```csharp
public readonly record struct ZLinkLocationOwnerToken(
    string OwnerId,         // host owner identity
    long LeaseGeneration);  // process lifecycle fence for the same OwnerId
```

`ZLinkOwnerLeaseReadResult.Found`, the result of reading the lease, returns the token
and timestamp together.

```csharp
public abstract record ZLinkOwnerLeaseReadResult
{
    private protected ZLinkOwnerLeaseReadResult() { }

    public sealed record Found(
        ZLinkLocationOwnerToken Token, // the current exact owner identity and generation
        DateTimeOffset LeaseExpiresAt, // store-relative expiration time
        DateTimeOffset StoreNow)       // provider-relative time used to judge expiry
        : ZLinkOwnerLeaseReadResult;

    public sealed record Missing : ZLinkOwnerLeaseReadResult;
}
```

<a id="fencing-deadline"></a>
### Fencing Deadline

The final time point by which a Server that failed to renew its owner lease must
stop accepting new business messages.

| Item | Content |
|---|---|
| Shape | Absolute lease-derived deadline |
| .NET notation | No public standalone type. The computed result is a `DateTimeOffset` time point. |
| Public composition | A single time point reflecting the fencing margin against the last valid owner lease deadline. |
| Creation/management | Computed by the framework from the last confirmed valid lease and location options. |
| Lifetime | Refreshed whenever a new valid lease is confirmed. Once the deadline is reached, new business admission closes regardless of Store failure grace. |

## 9. Network Listener

<a id="network-listener"></a>
### Network Listener

A transport component in the current process that binds a network endpoint and
accepts remote connections.

| Item | Content |
|---|---|
| Shape | Runtime transport component |
| .NET notation | Process default is `IZLinkNetworkOptions`; listeners are configured by the RouteMesh/ClientServer/fanout/STREAM builders. |
| Public composition | Has BindHost, a configured or allocated port, AdvertiseHost, the actual advertised endpoint, and topology identity. |
| Lifetime | Kept from a successful bind until the listener is closed via drain/shutdown. |

<a id="bind-host"></a>
### BindHost

The address specifying which local network interface on the current host a listener
accepts connections on. A wildcard address can be used.

| Item | Content |
|---|---|
| Shape | Host/address configuration value |
| .NET notation | `string` |
| Public composition | A single host name or IP address. The process default is `127.0.0.1`. |
| Creation/management | Specified by the application, either the process default or a listener override. |
| Lifetime | Fixed before the listener binds. `0.0.0.0` and `::` can be used but aren't emitted as the remote advertised address. |

<a id="advertise-host"></a>
### AdvertiseHost

The host or address a remote process actually uses to connect to the listener. A
wildcard address that doesn't pin down where the remote should connect can't be
used.

| Item | Content |
|---|---|
| Shape | Host/address configuration value |
| .NET notation | `string` |
| Public composition | A single host name or IP address resolvable from the remote side. |
| Creation/management | Specified by the application, or the framework uses the same host as a non-wildcard BindHost. |
| Lifetime | Fixed before the descriptor is published. A listener restart with a changed value uses a new lifecycle generation. |

<a id="wildcard-address"></a>
### Wildcard Address

A bind address, like `0.0.0.0` or `::`, that accepts connections on multiple local
network interfaces. Usable for local BindHost but not for AdvertiseHost.

| Item | Content |
|---|---|
| Shape | Special bind address value |
| .NET notation | `string` |
| Public composition | The exact value IPv4 `0.0.0.0` or IPv6 `::`. |
| Lifetime | Usable only as local bind input; can't remain in the descriptor or a manual remote endpoint. |

<a id="advertised-endpoint"></a>
### Advertised Endpoint

The connection address provided to a remote process, combining AdvertiseHost with
the actual bound port. Can't be left with a wildcard host or port `0`.

| Item | Content |
|---|---|
| Shape | A composite connection address of host and port |
| .NET notation | Public descriptors use `string Endpoint` |
| Public composition | `AdvertiseHost + actual bound port`. Additional format, such as a URI scheme, is defined by that transport's contract. |
| Creation/management | Fixed by the framework after reading the actual port from the listener bind, and recorded into the per-topology descriptor. |
| Lifetime | Kept for the listener's lifecycle. Published with a new lifecycle generation if the host or actual port changes. |

<a id="routing-id"></a>
### Routing ID

A byte value identifying a MeshNode within the same RouteMesh. In automatic
discovery, the framework generates a new value per lifecycle; in manual topology, an
explicit fixed RID can be used.

| Item | Content |
|---|---|
| Shape | Opaque byte identifier |
| .NET notation | `RoutingId` |
| Public composition | A full RID is at most 255 bytes. Core's raw-socket automatic RID is a 16-byte binary UUID v4. The framework's automatic RID, which provides a diagnostic prefix, is made of a prefix plus a 36-character lowercase canonical UUID v4. |
| Creation/management | Generated by the framework per lifecycle in automatic mode. On an active conflict, it doesn't generate a new UUID or a second claim. In manual topology, the application can specify a fixed RID. |
| Lifetime | Unchanged for a MeshNode's lifecycle. A replacement lifecycle uses a new Automatic RID even at the same endpoint. |

Transport RID and Spot ID issuance format and namespace boundaries follow the
[system-wide Routing ID policy](10-network-listener-identity.en.md#7-system-wide-routing-id-policy).

<a id="routing-id-prefix"></a>
### Routing ID Prefix

A diagnostic string prepended to an automatic MeshNode RID. Not used as application
identity, placement, shard, or a host name that survives a restart.

| Item | Content |
|---|---|
| Shape | Optional diagnostic string |
| .NET notation | `string?` |
| Public composition | ASCII `[A-Za-z0-9._-]`, 1-64 characters. The framework builds `prefix-<lowercase-canonical-uuid-v4>`. |
| Creation/management | Specified by the application in startup configuration and used by the framework when generating an Automatic RID. |
| Lifetime | Used only for that RID's generation. Not interpreted as a stable application identity or placement key. |

<a id="csprng"></a>
### CSPRNG

A cryptographically secure pseudo-random number generator whose next value is hard
to predict. The framework uses the platform's cryptographic random API to generate
the random bits of a UUID v4.

| Item | Content |
|---|---|
| Shape | Framework-internal randomness capability |
| .NET notation | No public framework type. The implementation uses the platform's cryptographic random API. |
| Public composition | Generates the random bits of a UUID v4, excluding the version/variant bits. The UUID's public representation is a lowercase canonical string, or Core's 16-byte binary value. |
| Lifetime | Generates a new value per RID-generation operation; doesn't expose random state to the application. |

<a id="routing-id-conflict"></a>
### RoutingIdConflict

The result of confirming, when claiming a framework-auto-issued RID, that an active
identity is already using it. A UUID collision isn't treated as a normal operating
condition, so a new UUID isn't generated and retried.

| Item | Content |
|---|---|
| Shape | Startup configuration failure |
| .NET notation | `ZLinkConfigurationException` |
| Public composition | Provides a description that an active transport descriptor owner claim conflicted. Doesn't include the conflicting owner token. |
| Lifetime | Ends that startup operation with a terminal failure; doesn't generate a new UUID within the same operation. |

<a id="spot-id-conflict"></a>
### SpotIdConflict

The result of confirming that an Entry/User/Instance Spot identity claim is already
in use within the global Spot ID namespace. The framework doesn't overwrite the
existing claim or retry the same operation with a new UUID.

| Item | Content |
|---|---|
| Shape | Startup or create failure |
| .NET notation | Startup uses `ZLinkConfigurationException`; exclusive create uses `ZLinkFrameworkErrorKind.AlreadyExists` |
| Public composition | Describes that a global Spot ID claim conflicted. Doesn't include the conflicting owner token. |
| Lifetime | Ends that startup or create operation with a terminal failure. |

## 10. STREAM Session And Actor Binding

<a id="stream-session"></a>
### STREAM Session

A server-side execution unit kept from accepting one STREAM client connection until
it closes. Typed packet handlers, request correlation, backpressure, and close
handling are all tied to this unit.

| Item | Content |
|---|---|
| Shape | Runtime session object |
| .NET notation | `IZLinkSession`; context is `IZLinkSessionContext` |
| Public composition | The context provides `SessionId`, an optional `RoutingId`, local/remote address, an outbound client, Actor binding, and a handler registry. |
| Creation/management | The framework creates a session instance and runs lifecycle callbacks when accepting a STREAM connection. |
| Lifetime | Kept from accepting the connection until disconnect/close finishes. A reconnection doesn't reuse the previous session identity or binding state. |

<a id="binding-token"></a>
### Binding Token

A value identifying the binding between an Actor and the current STREAM session, and
distinguishing late-arriving work from a previous session after a reconnect.

| Item | Content |
|---|---|
| Shape | Opaque one-binding token |
| .NET notation | Not exposed to the application as an independent public scalar type. The binding API owns the internal token. |
| Public composition | A single opaque token. Internal encoding isn't part of the public contract. The binding relationship is verified together with the exact `ActorRef`, current authority/lease generation, session identity, and binding generation. |
| Creation/management | Issued as a new token by the current Actor owner on a successful bind or rebind. |
| Lifetime | Invalidated by rebind, unbind, session close, or a generation change. A previous token can't be used for dispatch/reply/push/close. |

<a id="binding-route"></a>
### Binding Route

The current Actor owner delivery path a session owner keeps for a specific Actor
binding. On a successful bind, a route built from the verified `ActorRef` location is
stored, and relay/disconnect notification and Actor push use this stored route.
Route isn't re-selected by re-querying the Location Store on every message.

During Actor relocation, only the Actor of the same `ObjectGeneration` that's
restored on the target and has completed the owner/membership commit gets its route
updated. The target runtime asks the session owner to update the moved Actor's
binding route and the bound-session's current Actor location snapshot together, to
the target owner and target MeshName/NodeRid. ActorId/ObjectGeneration are kept, and
a new incarnation must explicitly rebind. The route, location snapshot, and physical
STREAM connection of other Actors on the same Session that aren't part of the
relocation are kept. The Location Store and Relocation Store don't store or update the
binding route.

<a id="binding-route-ack"></a>
### Session Actor Location Update Acknowledgement

The `sessionActorLocationUpdateResMsg` by which a session owner stores the new
binding route and the current Actor location snapshot after relocation, and confirms
it has verified that late packets and pushes from a previous owner generation aren't
applied to the current binding. The snapshot has the same ActorId/ObjectGeneration
and target MeshName/NodeRid. This response is used to stop location-update resends —
it isn't a signal allowing the target Actor to process messages or complete a Join.

The response result is `Applied`, `AlreadyApplied`, `Stale`, or
`SessionOrBindingClosed`. The first two mean the requested location was applied. The
latter two mean a more recent location exists, or the Session/binding closed, so the
previous location wasn't applied. The exact wire values and the conditions for
removing a Message Follow route are defined by
[Session-Actor dispatch §5.1](20-session-actor-dispatch.ko.md#51-session-actor-위치-갱신-message).

<a id="binding-generation"></a>
### Binding Generation

An owner-local value distinguishing the order in which a binding was replaced within
the same session owner process lifecycle. Not compared in magnitude against a
different owner's or a restarted process's value.

| Item | Content |
|---|---|
| Shape | Owner-local monotonic generation |
| .NET notation | A `ulong` contract value not exposed directly to the application |
| Public composition | A single generation value of at least 1. Binding identity also uses the session owner node RID and node lifecycle generation together. |
| Creation/management | Incremented by the session owner following bind/rebind order. |
| Lifetime | Compared only within the same session owner process lifecycle. Values from a different owner or process lifecycle aren't compared in magnitude. |

<a id="authority-owner-generation"></a>
### AuthorityOwnerGeneration

A provider-issued value indicating the order in which the authority owner changed
within the same object incarnation. Different from ObjectGeneration, which
distinguishes whether an object was re-created after deletion.

| Item | Content |
|---|---|
| Shape | Provider-issued monotonic generation |
| .NET notation | `ulong AuthorityOwnerGeneration` |
| Public composition | A single integer in the range `1..long.MaxValue`. |
| Creation/management | Issued by the Location Store provider's global durable counter on initial reserve and on a `NewOwner` transition. |
| Lifetime | A new value is used when the owner changes within the same ObjectGeneration. Doesn't wrap at the maximum value — fails with `GenerationExhausted` instead. |

<a id="owner-lease-generation"></a>
### OwnerLeaseGeneration

A provider-issued value distinguishing the host process lifecycle the current object
owner belongs to. Prevents work from a previous process, under the same owner
identity, from being accepted as current work after a restart.

| Item | Content |
|---|---|
| Shape | Provider-issued owner lifecycle generation |
| .NET notation | `long OwnerLeaseGeneration`; in an owner token it's `ZLinkLocationOwnerToken.LeaseGeneration` |
| Public composition | A single positive generation value that forms the exact owner token together with `OwnerId`. |
| Creation/management | Issued by the Location Store provider on a new owner lease claim. |
| Lifetime | Kept for that host process owner lease's lifecycle. A restart under the same OwnerId uses a new value. |

<a id="session-sequence"></a>
### Session Sequence

A value representing the order of ingress messages accepted on one STREAM session.
In Actor handoff, the last accepted sequence is used as a barrier so the previous
owner and the new owner don't process the same message together.

| Item | Content |
|---|---|
| Shape | Per-session monotonic sequence |
| .NET notation | A `ulong` contract value not exposed directly to the application |
| Public composition | A single ingress-order value in the range `1..long.MaxValue`. A separate axis from binding generation. |
| Creation/management | Incremented by the session owner in the order ingress messages are accepted. |
| Lifetime | Compared only within one STREAM session. The Actor handoff barrier fixes the last accepted sequence as the high-water mark. |

## 11. Stream Connector

<a id="stream-connector"></a>
### Stream Connector

A client library that connects to the server framework's STREAM model to exchange
packets. Distributed separately from the server framework package, providing
transport, codec, reconnect, and dispatch suited to the client's execution
environment.

| Item | Content |
|---|---|
| Shape | Client runtime component |
| .NET notation | `IZlinkStreamConnector`, `ZlinkStreamConnectorFactory`, `ZlinkStreamConnectorOptions` |
| Public composition | Provides connection lifecycle, typed send/request, wait/handler registration, a pending dispatch queue, and runtime events. |
| Lifetime | Kept from factory creation through `Close` and `DisposeAsync` completion. |

<a id="stream-packet"></a>
### Stream Packet

A STREAM transport unit combining header information — message kind and an optional
packet name — with a payload. The header also includes the value linking a request
to its reply.

| Item | Content |
|---|---|
| Shape | Binary frame with a composite header |
| .NET notation | No public raw-packet type. The .NET Connector wraps it with typed send/request calls and `ZLinkMessage`-family values. |
| Creation/management | Encoded/decoded by the connector runtime; the application doesn't directly assemble or modify the header. |
| Lifetime | Kept for the duration of sending/receiving one STREAM transport frame and matching a pending request. |

| Wire composition | Format |
|---|---|
| Frame prefix | `u16 header_len`, `u32 payload_size` |
| Fixed header | `format_marker = 0xF2`, `kind u8`, `codec u8`, `flags u8` |
| Request sequence | `request_seq u64` when the flag is set; `0` isn't used. |
| Packet name | `u8 name_len + UTF-8 bytes`, at most 255 bytes. Length `0` for Response and Error. |
| Metadata | `u16 meta_len + encoded metadata` when the flag is set |
| Correlation ID | `u8 length + ASCII bytes` when the flag is set |
| Flow | A 36-byte `flow_id` and a 1-byte `flow_origin` are both present when the flag is set. |
| Payload | `payload_size` bytes following the header |

Every multi-byte integer uses network byte order.

<a id="dispatch-mode"></a>
### Dispatch Mode

A Connector setting deciding whether receive callbacks run automatically in the
receive loop, or are explicitly pumped in an application-specified context. Game
engines default to `Manual` because of main-thread constraints.

| Item | Content |
|---|---|
| Shape | Closed Connector execution mode |
| .NET notation | The current common .NET exact interface has no independent public enum. As C# contract pseudocode it's the two values `Manual` and `Immediate`. |
| Public composition | `Manual` has the application explicitly pump the dispatch queue; `Immediate` runs the callback inline on the receive path. |
| Creation/management | Specified by the application in Connector options; a game engine's default is `Manual`. |
| Lifetime | Kept for the Connector instance's configuration. |
