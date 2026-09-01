# ZoneWorld Sample Scenario

[Sample List](../README.en.md)

> ZoneWorld bundles a player Actor moving across zone boundaries and an operations console into
> one sample, showing that the Framework provides global Spot routing, cross-node relocation,
> bound sessions, Logical Multicast, and classic fanout so the Application can focus on world
> rules and desired state.

## 1. Purpose And Scope

This sample covers a world split into four logical zones, where a player moves across boundaries,
border snapshots are delivered to adjacent zones, and an Ops console manages node status and
maintenance mode. Gateway terminates the game browser STREAM and binds the player Actor to the
current session. ZoneNode provides the zone Spot and Player Actor. Ops provides the control STREAM,
runtime event observation, and fanout to every node.

The Framework's responsibility is global ZoneId Spot routing, Actor membership and cross-node
relocation, Message Follow, bound session routes, Logical Multicast subscription, classic fanout
transport, and runtime event delivery. The Application owns coordinates/zone rules, border-snapshot
expiry, bot paths, NodeId desired state, and UI policy.

The .NET and Node.js servers use the same wire contract and share a TypeScript browser client. The
headless runner runs the per-server self-check, and the browser client verifies the actual WS/WSS
transport and screen flow.

At start, the following conditions are assumed.

- The coordinate range and the IDs of the four zones are fixed.
- Gateway, two ZoneNodes, and Ops have completed readiness.
- A Location Store, Relocation Store, and maintenance store are prepared per run.
- Four X-patrol bots and four Y-patrol bots are generated with a deterministic seed.

The scope covers joining, movement, border sync, relocation, bots, node observation,
all-node announcements, and maintenance changes. The following are excluded.

- Combat, items, economy, and cross-zone game aggregation
- The client changing local state as authoritative
- Using the transport RID as a player, zone, or node address
- Automatic crash failover after a Ready owner failure
- Product-grade authentication and access control for the browser UI

## 2. Requirements

### 2.1 World Functional Requirements

| Item | Criteria |
|---|---|
| Coordinates | 0 <= X < 100, 0 <= Y < 100 integers |
| Zones | Four quadrants: zone-nw, zone-ne, zone-sw, zone-se |
| Adjacency | Only zones sharing an edge are adjacent; diagonals are excluded |
| Border band | Players within 10 of the border are included in the adjacent zone snapshot |
| Tick | 0 at zone Spot creation, +1 every 100ms after |
| Movement | Max 5 per axis in one `MoveMsg` |
| Start | `JoinWorldRes` returns (25, 25), zone-nw |
| Player state | The Actor owns the authoritative X, Y, ZoneId state; the Zone Spot keeps a copy |
| Bots | Two human-shaped bots per zone, 8 total, no bound session |

### 2.2 Operational And Quality Requirements

- Move rejection order is fixed as OutOfRange → TooFar → DiagonalCrossing → ZoneMaintenance.
- `ZoneStateNotify.Players` prioritizes the own-zone value and sorts by PlayerId in ascending
  UTF-8 byte order.
- An adjacent zone snapshot is replaced by the latest Tick per FromZoneId, and is removed if no new
  snapshot arrives for 3 ticks.
- A cross-node zone move keeps the same PlayerId and ObjectGeneration, changing only the owner
  generation. The client WebSocket connection is kept on the normal path where the session route
  update is applied within the seal timeout; past the timeout the Framework closes the physical
  connection, and the client observes a disconnect and reconnects (§7.5).
- Border sync and announce are publishes, and target-handler completion isn't used as a success
  criterion.
- Maintenance mode records desired state to the store and notifies every node via fanout. The
  target Spot's `OnActorJoin` admission is the **sole** final decider; source/Entry maintenance
  caches are observation/optimization only and never produce a client-facing terminal result.
  During maintenance only same-zone movement is allowed (moving to a different zone on the same
  NodeId is also rejected).
- Runtime node status is observed via runtime events and explicit reports, not polling.
  Registered is based on the ZoneNode's explicit report and is observed as false once 15 seconds
  (three 5-second report intervals) pass after the last report. A crashed node cannot send a
  false report, so this TTL is the only false-transition rule.
- A Ready owner failure is not automatic replacement, and that operation ends as Unavailable.

### 2.3 Surface Selection Criteria

| Task | Framework Surface | Reason |
|---|---|---|
| Observing node registration/connection changes | Runtime event | Changes are delivered without sending a request to the target node. |
| All-node announcement | Classic fanout | The publisher doesn't manage a subscriber list. |
| Maintaining a specific NodeId | Desired state + fanout | NodeId is an application label, not a transport RID. |
| Adjacent zone snapshot | Logical Multicast | Publishes to per-topic adjacent-zone subscribers. |
| Push to a specific player | Bound session | The Actor's current binding resolves the connection location. |

## 3. System Configuration And Topology

The base topology only expresses the placement of Client and server components and their
connections. Redis and the maintenance store are described in the resource table, and the time
order of movement/publish is described in the §7 sequence diagrams.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-topology-en.html" title="System configuration and topology" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-topology-en.html" target="_blank">↗ View larger</a></p>

- Only Gateway provides the player-facing game STREAM, and only Ops provides the control STREAM.
- ZoneNode A/B run with the same executable capability, registering the Zone Spot factory
  (stable type `zoneworld.zone`, four Spot instances by ZoneId), the Player Actor factory
  (stable type `zoneworld.player`), the zone Channel, and the report channel. Every language
  registers these canonical stable type strings identically (actorJoin does not carry the stable
  type on the wire — it is resolved from the Location Store authority row, so diverging names
  break cross-language joins).
- Each ZoneNode declares a Zone Spot capacity of 2. The four zones are therefore spread 2/2
  across the two nodes by capacity, and any 2/2 split of the 2x2 grid guarantees an adjacent
  zone pair with different owners. The runner never assumes a zone→NodeId mapping: it discovers
  the actual owner layout via Ops probes and picks the cross-owner boundary from that. Fixtures
  or tests that pin a specific zone to a specific NodeId are forbidden.
  Each ZoneNode's bootstrap claims its first zone, then **prefers an adjacent zone** for its
  second claim (contiguous-region preference). A 2/2 split therefore always forms two contiguous
  regions, so both a cross-owner adjacent pair and a same-owner adjacent pair (ZW-E4's
  precondition) deterministically exist — a diagonal split ({nw,se}/{ne,sw}) has no same-owner
  adjacent pair and would make E4 unsatisfiable, so it is excluded. The preference is only the
  claim-attempt ORDER, not owner computation; placement remains owned by Framework capacity.
- **The readiness line is fixed.** When a ZoneNode finishes preparing it writes exactly one line
  to standard output: `topology=ready node=<NodeId> zones=<comma-joined ZoneIds>`. With no zone,
  nothing follows `zones=`. The runner waits on this line before moving to the next step, so a
  per-language string makes a shared runner procedure impossible. No extra field is appended.
- **Bootstrap announces readiness only after it holds two zones.** A ZoneNode claims at startup
  and repeats until its own census holds two zones. Announcing readiness before that lets
  `ZW-C1` — "the console observes both ZoneNodes as Registered and Connected" — pass a node that
  holds no zone, which changes what the assertion means. Registering only the factory and
  creating the Zone Spot on the first request does not satisfy this.
- **Claim retry is `250 ms` apart, at most `120` attempts.** The other ZoneNode may not be up
  yet, leaving capacity free, so a node retries instead of failing immediately. A node that
  exhausts all 120 attempts without holding two zones fails startup — it never quietly announces
  readiness with no zone. The values are fixed because a per-language interval or count makes the
  same scenario fail at a different moment in each language, which splits the verdict.
- **A crash-replacement process announces readiness without claiming a zone.** §7.5 defines a
  crash replacement as "becoming able to accept new objects" and forbids restoring the previous
  owner's objects. Only in this case is readiness announced with zero zones, and the runner turns
  that intent on explicitly. The setting is named `allowEmptyZoneSet`, spelled to each language's
  naming rule (`allow_empty_zone_set`, `allowsEmptyZoneSet`). On this path a node stops retrying
  and announces readiness once `attempt` reaches `8` with an empty census. A normal startup never
  takes this path.
- `zoneworld.mesh` carries the ChannelName, Spot/Actor direct messages, and Logical Multicast.
- `zoneworld.broadcast` is a classic fanout publisher/subscriber connection independent of the mesh.
- Location Store placement selects the owner of objects such as Zone Spots and Player Actors.
  NodeId and the transport RID are separate domains.
- Only the Gateway and Ops endpoints are provided to the Client; the ZoneNode endpoint isn't
  exposed.

| Resource | Responsibility | Preparation |
|---|---|---|
| Location Store | Peer descriptor, ZoneId Spot authority, and Actor location | Shared Redis, per run |
| Relocation Store | Operation recovery records for Player Actor relocation (post-relocation pending-request terminal records) | A per-run Redis keyspace with a provider and key prefix separate from the Location Store |
| Maintenance store | Desired state per NodeId | Shared Redis keyspace, per run |
| Zone state | Actor coordinate copy, border snapshot, and tick | Zone Spot |
| Player actor state | Coordinates, zone, and bot direction | Player Actor relocation adapter |
| Runtime evidence | Node status, alerts, and relocation probes | Ops runner |

## 4. Roles And Responsibilities

| Role | Count | Responsibility | Separation Reason And Ownership |
|---|---:|---|---|
| Game Browser | 1+ | JoinWorld, Move, checking state notify and announce | Updates screen state from server push alone. |
| Ops Browser | 1 | Node watch, announce, maintenance, and diagnose | Separates game domain state from operational desired state. |
| Gateway | 1 | Game STREAM, Player Actor binding, relay, and push | Separates browser connection lifetime from the zone owner. |
| ZoneNode | 2 | Entry Spot, four Zone Spots, Player Actor, bot timer, and local report | Distributes zone objects across multiple owner candidates. |
| Ops | 1 | Ops STREAM, runtime event collection, fanout publish, and maintenance store | Doesn't hardcode the node list into publisher code. |
| Zone Spot | 1 per ZoneId | Player copy, border snapshot, tick, and admission | The single owner of zone view state. |
| Player Actor | 1 per PlayerId | X, Y, ZoneId authority, and movement/relocation | Keeps client-input authority in one place. |

NodeId is an application label not used to compute the zone owner. The MeshNode RID is
auto-issued by the Framework as a prefix plus a UUID; no fixed RID is configured.

## 5. Framework Elements Used And Why

| Behavior Needed | Element Chosen | Reason And Contract Basis |
|---|---|---|
| Find the current zone owner by ZoneId. | Global Spot message | The Framework resolves the global SpotId authority. [Interaction Model §2](../../spec/server/00-foundation/04-interaction-model.en.md) |
| Find an actor by PlayerId. | Global Actor message | Doesn't expose the Actor location or current owner as an application route. [Actor model](../../spec/server/03-spot-actor/04-actor-model.en.md) |
| Use zone join as a cross-node move. | Actor Join + relocation | When the target owner differs, the Framework relocation unit moves the actor. The single authority for the full relocation order (owner transition, relay, target queue, CAS) is [Relocation flow](../../spec/server/05-location-relocation/04-relocation-flow.en.md); target admission, membership, and lifecycle are owned by [Spot and Actor membership §4.2](../../spec/server/03-spot-actor/05-spot-actor-membership.en.md#42-the-order-for-joining-an-actor-to-a-spot-on-a-different-node) |
| Deliver a message to the previous owner during a move. | Message Follow | Uses the committed target route and doesn't automatically resubmit a failed operation to a different owner. [Object routing §2.4](../../spec/server/03-spot-actor/08-routing.en.md#25-a-message-arriving-at-a-previous-owner-route) |
| Deliver a snapshot to an adjacent zone. | Logical Multicast | Expresses the boundary via topic and target subscription. [Interaction Model §5](../../spec/server/00-foundation/04-interaction-model.en.md#5-spot-logical-multicast) |
| Send all-node announcements/maintenance. | Classic fanout | The publisher doesn't manage the node list. [Interaction Model §6](../../spec/server/00-foundation/04-interaction-model.en.md#6-classic-fanout) |
| Observe node status. | Runtime monitoring event | Ops collects status changes and local reports. [Runtime monitoring](../../spec/server/06-observability/01-runtime-monitoring.en.md) |
| Keep the actor connection alive. | Bound STREAM session | Keeps the same connection during relocation, only updating the binding location. [Failure policy §6](../../spec/server/05-location-relocation/06-failure-failover-policy.en.md#6-session-and-binding) |
| Avoid RID collisions. | `SetRoutingIdPrefix` zn | Separates the application NodeId/ZoneId from transport identity. [MeshNode spec](../../spec/server/03-spot-actor/03-mesh-node.en.md) |

The Player Actor factory registers a `PreserveStateWith` relocation adapter. Its Capture/Restore
payload preserves only Application-owned state such as coordinates, ZoneId, bot direction, and the
last applied movement ID. It doesn't include the queue, accepted journal, logical timer, and
membership preserved by the Framework, nor the owner fence the Framework advances on every owner
change. Zone Spot factories that don't move select
`DisableRelocation`.

## 6. Message Contract

ZoneWorld uses a typed JSON codec. The declarations below are the JSON wire names and
optional/null semantics that .NET, Node.js, and the shared TypeScript browser must keep.

The player-facing wire is **logical-only**: game messages never carry a NodeId, transport RID,
relocation-occurrence flag (such as `transferred`), or owner information. Screens that need such
physical observation (HUD/demo) obtain it from the Ops contract (WatchNodes/Diagnostics). No
client, including the shared browser, adds physical fields to these declarations.

Business failures are observed only through the following typed mapping (no free-form strings
that would expose per-language exception text):

| Failure | Client observation |
|---|---|
| Move rejection (OutOfRange/TooFar/DiagonalCrossing/ZoneMaintenance) | the matching code in `MoveRejectedNotify.reason` |
| JoinWorld zone-admission rejection (maintenance etc.) | a typed code in `JoinWorldRes.error` (e.g. `ZoneMaintenance`) |
| Any other Framework Join/request failure | the public failure kind name from [Spot and Actor membership §4](../../spec/server/03-spot-actor/05-spot-actor-membership.en.md) **verbatim** in the `error` field (e.g. `NotFound`, `CapacityExceeded`, `InternalFailure`, `DataLost`, `InvalidOperation`, `ShuttingDown`) — no strings outside this closed set |
| Operation ended by target owner crash | `JoinWorldRes.error` / the request's `error` = `Unavailable` |
| Request deadline exceeded | the request's `error` = `DeadlineExceeded` |
| Session route-update timeout | WebSocket close (no message; §7.5) |

### 6.1 Game STREAM Messages

```text
message PlayerView {
  playerId: string
  x: int32
  y: int32
  zoneId: string
  isBot: bool
}

message JoinWorldReq {
  playerId: string
}

message JoinWorldRes {
  playerId: string
  zoneId: string
  x: int32
  y: int32
  error?: string | null
}

message MoveMsg {
  x: int32
  y: int32
}

message ZoneStateNotify {
  zoneId: string
  tick: int64
  players: PlayerView[]
}

message ZoneChangedNotify {
  playerId: string
  zoneId: string
}

message WorldAnnounceNotify {
  announcementId: string
  text: string
}

message MoveRejectedNotify {
  reason: string
  x: int32
  y: int32
}
```

`MoveMsg` is a one-way send with no response. `ZoneChangedNotify` only announces the logical
ZoneId change and doesn't expose whether a physical relocation occurred or the owner RID.

### 6.2 Ops STREAM Messages

```text
message NodeView {
  nodeId: string
  registered: bool
  connected: bool
  maintenance: bool
  zones: string[]
  playerCount: int32
}

message WatchNodesReq {}

message WatchNodesRes {
  nodes: NodeView[]
}

message NodeStatusNotify {
  nodeId: string
  registered: bool
  connected: bool
  maintenance: bool
  zones: string[]
  playerCount: int32
}

message NodeAlertNotify {
  nodeId: string
  kind: string
  detail: string
  occurredAt: string
}

message AnnounceWorldReq {
  text: string
}

message AnnounceWorldRes {
  announcementId: string
}

message SetMaintenanceReq {
  nodeId: string
  enabled: bool
}

message SetMaintenanceRes {
  nodeId: string
  enabled: bool
  zones: string[]
  error?: string | null
}

message NodeDiagnosticsReq {
  nodeId: string
}

message NodeDiagnosticsRes {
  nodeId: string
  zones: string[]
  playerCount: int32
  maintenance: bool
  error?: string | null
}
```

NodeId is the application identifier Ops displays. `NodeDiagnosticsReq` and `SetMaintenanceReq`
use the NodeId that appears in the current node report and don't accept an RID as input.

### 6.3 Internal Routing, Border, And Probe Messages

```text
message WorldAnnounceEvent {
  announcementId: string
  text: string
}

message NodeMaintenanceChangedEvent {
  nodeId: string
  enabled: bool
}

message DeliverAnnounceMsg {
  announcementId: string
  text: string
}

message BotTickMsg {}

message EnterWorldReq {
  x: int32
  y: int32
  isBot: bool
  dirX?: int32
  dirY?: int32
}

message EnterWorldRes {
  zoneId: string
  x: int32
  y: int32
  error?: string | null
}

message ReportSpotEventMsg {
  nodeId: string
  kind: string
  detail: string
  occurredAt: string
}

message ReportNodeStatusMsg {
  nodeId: string
  zones: string[]
  playerCount: int32
  maintenance: bool
}

message ZoneBorderEvent {
  fromZoneId: string
  toZoneId: string
  tick: int64
  players: PlayerView[]
}

message EnterZoneReq {
  playerId: string
  x: int32
  y: int32
  isBot: bool
  initialEntry: bool
}

message EnterZoneRes {
  zoneId: string
  error?: string | null
}

message UpdatePositionMsg {
  playerId: string
  x: int32
  y: int32
  isBot: bool
}

message DeliverZoneStateMsg {
  zoneId: string
  tick: int64
  players: PlayerView[]
}

message DeliverWorldAnnounceMsg {
  announcementId: string
  text: string
}
```

`WorldAnnounceEvent` and `NodeMaintenanceChangedEvent` are classic fanout publish payloads.
`ZoneBorderEvent` is a Logical Multicast publish payload. `ReportSpotEventMsg` and
`ReportNodeStatusMsg` are one-way messages ZoneNode sends to the Ops channel. `MessageFollowProbe`
and `ActorLocationProbe` are runner-only evidence and are not part of the browser application
contract.

## 7. Business Flow

### 7.1 Joining And Moving Within The Same Zone

The starting state is that Gateway, the two ZoneNodes, and Ops have completed readiness, and the
browser has connected a STREAM to Gateway. JoinWorld starts at (25,25) in zone-nw. When the actor
moves within the same zone, the Actor updates the coordinates and sends `UpdatePositionMsg` to the
Zone Spot to update the copy.

A zone join is a Framework Actor Join, so it does not complete synchronously inside a handler.
The Actor registers the join with `Defer()`, ends the current handler normally, and the join
result arrives in a completion callback ([Spot and Actor membership §3](../../spec/server/03-spot-actor/05-spot-actor-membership.en.md)).
`JoinWorldRes` is therefore sent from the join completion callback — **a successful JoinWorldRes
means target zone admission has completed** is the normative meaning in this scenario, and an
implementation that produces the JoinWorldRes terminal from pre-admission state (such as a
cache) is non-conforming.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-join-move-en.html" title="Joining and moving within the same zone" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-join-move-en.html" target="_blank">↗ View larger</a></p>

### 7.2 Border Crossing And Relocation

If the target zone owner is the same, only membership changes; if different, the same Player Actor
materializes at the target owner, which is a relocation. The Application doesn't distinguish the
two cases by NodeId — both use the `EnterZoneReq`/`EnterZoneRes` request/reply pair.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-relocation-en.html" title="Border crossing and relocation" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-relocation-en.html" target="_blank">↗ View larger</a></p>

Relocation keeps the ActorId and ObjectGeneration and only changes the owner generation. A
one-way or request message that arrives at the previous owner during relocation Follows to the
committed target. The source doesn't re-query the Location Store or automatically resubmit the
same operation to a different owner while Following.

### 7.3 Border Snapshots And Bots

Each Zone Spot builds a `ZoneStateNotify` from its own zone and adjacent snapshots every tick, and
publishes `ZoneBorderEvent` to a per-adjacent-zone topic. Receivers keep only the latest Tick per
FromZoneId and remove it if it isn't refreshed for 3 ticks. If the same PlayerId appears in both the
own zone and a border snapshot at once, the own-zone value is used.

A bot is the same Player Actor type as a human and has no bound session. Each of the four zones has
one X-direction bot and one Y-direction bot, moving 3 cells every 500ms `BotTickMsg`. When a move is
rejected, the direction reverses. The initial coordinates and directions are fixed as follows.

| PlayerId | Start Coordinate | Direction | Border Effect |
|---|---|---|---|
| bot-nw-x | (10,15) | (+1,0) | X-border cross-node relocation |
| bot-nw-y | (15,10) | (0,+1) | No X border |
| bot-ne-x | (90,15) | (-1,0) | X-border cross-node relocation |
| bot-ne-y | (85,10) | (0,+1) | No X border |
| bot-sw-x | (10,85) | (+1,0) | X-border cross-node relocation |
| bot-sw-y | (15,90) | (0,-1) | No X border |
| bot-se-x | (90,85) | (-1,0) | X-border cross-node relocation |
| bot-se-y | (85,90) | (0,-1) | No X border |

### 7.4 Ops Observation, Announce, And Maintenance

Ops converts runtime events and explicit reports from ZoneNode into `NodeStatusNotify` and
`NodeAlertNotify`. Here, a runtime event means the current-status query and change-observation
surface of [Runtime monitoring](../../spec/server/06-observability/01-runtime-monitoring.en.md), where each item is
a complete status, not a partial event. `WatchNodesRes`'s Registered and Connected are different
observations: Connected comes from the peer state of the runtime status observation, and
Registered comes from ZoneNode's explicit report, since the Framework topology status doesn't
expose a registration signal.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-ops-en.html" title="Ops observation, announce, and maintenance" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-ops-en.html" target="_blank">↗ View larger</a></p>

If the target zone owner has `maintenance=true`, the target Zone Spot's `OnActorJoin` admission
rejects with ZoneMaintenance. Only movement within the same zone is allowed (moving to a
different zone on the same NodeId is a new admission and is rejected too). Even if the fanout
cache is stale, target admission is the sole final decider and source/Entry caches never produce
a terminal. Ops records desired state to the maintenance store, so the maintenance state for the
same NodeId is restored after a ZoneNode restart.

This maintenance is application admission desired state and does not invoke
[Host relocation flow](../../spec/server/05-location-relocation/05-host-relocation-flow.en.md)'s
`Relocate(PlannedMaintenance)` — ZW-E is not a verification target for Spec 30 host relocation
(that coverage is owned by a separate harness).

### 7.5 Failure And Failover Boundary

If the Ready ZoneNode owner process terminates, the current Actor/Spot operation ends as
Unavailable. The Framework doesn't automatically create a new Actor incarnation on a different
ZoneNode. Planned relocation is a separate operation following the target-only Location Store CAS commit
rule, and is not crash failover.

If the bound-session route update is not applied within the seal timeout during relocation, the
Framework closes the physical connection. The client's observable result is a WebSocket close;
the client reconnects and performs JoinWorld again (rebinding to the existing Actor with the same
PlayerId). This failure path is observed by a §9.1 self-check item.

"Crash replacement" means starting a **new process (new transport RID)** under the same NodeId
so that new objects can be hosted. It is not automatic restoration or re-creation of the objects
the previous Ready owner held; their incomplete operations remain ended at the Unavailable
boundary.

## 8. Implementation Structure

The .NET and Node.js servers place `Client`, `Shared`, and `Server` in the same order and keep the
logical components below with the same responsibilities. The headless scenario and browser client
can live in different file locations, but the boundaries of Gateway, ZoneNode, and Ops, and the
placement of the zone state owner and relocation adapter, don't change.

<iframe class="zlink-diagram" src="/common/diagrams/sample-zoneworld-structure-en.html" title="Implementation structure — Client · Shared · Server" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-zoneworld-structure-en.html" target="_blank">↗ View larger</a></p>

| Logical Component | Responsibility Kept In Every Language | Dependency Direction And Forbidden Boundary |
|---|---|---|
| `Client/Program` | Composes the configuration/execution entry point for the headless runner and browser adapter. | Doesn't select the ZoneNode owner or runtime RID. |
| `Client/HeadlessScenario` | Runs join, move, border, bot, announce, maintenance, and §9 assertions. | Doesn't directly choose the ZoneNode owner or transport RID. |
| `Client/BrowserGame`/`BrowserOps` | Verifies the response and push of the game/ops screens using the same wire contract. | Doesn't create a separate message codec per browser. |
| `Shared/Configuration` | Fixes role, Mesh/fanout, zone fixtures, and the runner marker. | Doesn't treat NodeId and the MeshNode RID as the same value. |
| `Shared/JSON Contracts` | Owns the wire semantics of game, ops, internal routing, and runtime events. | Doesn't add .NET/Node-only fields to the common contract. |
| `Shared/WorldRules` | Computes coordinates, zones, rejection order, and border policy. | Doesn't reference Framework types or transport. |
| `Server/Gateway/Application` | Coordinates Player binding, relay, and client-facing result mapping. | Doesn't directly change zone state. |
| `Server/Gateway/Infrastructure` | Wires WebSocket/STREAM, handlers, and the push adapter. | Doesn't expose the frame codec or owner route to the application. |
| `Server/ZoneNode/Domain` | Computes the zone snapshot, player state view, and border rules. | Doesn't cache the ActorRef in state. |
| `Server/ZoneNode/Application` | Coordinates movement, admission, bot tick, and the before/after result of relocation. | Doesn't use NodeId as a Framework routing identity. |
| `Server/ZoneNode/Infrastructure` | Wires the Entry Spot, Zone Spot, Player Actor, relocation, border, and local report. | Doesn't use raw frames or a private runtime API. |
| `Server/Ops/Application` | Coordinates node watch, announcement, maintenance, and diagnostics results. | Doesn't directly change game domain state. |
| `Server/Ops/Infrastructure` | Wires the Ops stream, runtime event, fanout, and maintenance store. | Doesn't hardcode the node list into publisher code. |

WorldRules owns coordinates, zones, rejection order, and border policy. Gateway owns only the
stream transport and session binding. ZoneSpot owns the copy of actor coordinates, tick, and border
snapshot, and doesn't cache the ActorRef. PlayerActor owns the authoritative coordinate/ZoneId
state, the relocation adapter, and bound push. Ops owns NodeId desired state and runtime evidence.
The browser transport connects the platform WebSocket through the stream connector, and the
application doesn't re-implement the frame codec.

Language-specific implementations don't merge Gateway/ZoneNode/Ops into one server module, nor
duplicate Zone state into Gateway or Ops. .NET and Node.js can represent internal types differently,
but the same logical component and wire declaration must be findable. What can differ per language
is the host/browser adapter, async expression, and runtime event wrapper — relocation, border, bot,
fanout, and self-check order must match the common document.

.NET attributes, Java/Kotlin annotations, and Node.js decorators auto-register handlers through
declarative metadata scanning. C++ has no runtime reflection scanner, so it explicitly registers the
same handler set using compile-time types and a public builder. This difference only applies to the
registration method and doesn't change the message or processing responsibility.

## 9. Client Self-Check

### 9.1 Game Browser

1. Confirm `JoinWorldRes` returns zone-nw, (25,25).
2. Confirm the coordinates and ZoneId in `ZoneStateNotify` after a same-zone `MoveMsg`.
3. Confirm that out-of-range, over-distance, diagonal crossing, and maintenance rejection return
   the defined order and reason.
4. Confirm that two different players appear in the same `ZoneStateNotify` and the PlayerId
   UTF-8 byte order is correct.
5. Confirm border snapshots arrive only for adjacent zones and not for diagonal zones.
6. Confirm a border snapshot ignores tick reversal and is removed if not refreshed for 3 ticks.
7. Have the runner select an adjacent-zone pair with different owners and perform a cross-node
   move. Confirm the same ActorId and ObjectGeneration, a kept WebSocket binding, and
   `ZoneChangedNotify`. If no such pair exists, don't pass the release gate.
8. Immediately after relocation, send one-way and request probes on the previous owner route and
   confirm the operation id, generation, payload, and reply route are preserved. It's a failure if
   the source re-queries the Store or does a hidden retry while Following.
9. Confirm the deterministic movement and post-rejection direction reversal of the X-patrol and
   Y-patrol bots. Bots must not produce client push.

### 9.2 Ops Browser

1. Confirm Registered and Connected separately in `WatchNodesRes`.
2. Confirm `NodeStatusNotify` and `NodeAlertNotify` reflect runtime events and local reports.
3. Confirm the AnnouncementId arrives at the game client without duplication after
   `AnnounceWorldReq`. Fanout publish completion or subscriber handler completion isn't used as the
   client success condition.
4. Confirm `SetMaintenanceReq` changes only the selected NodeId and that desired state is recorded
   to the store.
5. Confirm that during maintenance, a new join to the target zone is rejected as ZoneMaintenance
   while movement within the same zone is allowed.
6. Confirm `NodeDiagnosticsReq` returns the latest zone list, player count, and maintenance.
7. After a graceful ZoneNode shutdown and restart, confirm a new transport RID with the same
   NodeId report.
8. Confirm via the Unavailable boundary that replacement after a ZoneNode crash is not automatic
   failover of the previous owner.

### 9.3 Routing ID Gate

- The MeshNode RID has the form `zn-<lowercase-canonical-uuid-v4>`, with no fixed RID
  configuration or `SetRoutingId` call.
- ChannelName, Spot, and Actor don't create separate transport RIDs.
- Global ZoneId routing operates independently of NodeId across process start order, graceful
  replacement, and crash replacement. Here "operates" means the new process reports the same
  NodeId under a new RID and subsequent **new** object creation/routing is normal; it does not
  mean automatic recovery of the objects owned before the crash (§7.5).
- The observational OwnerNodeRid is only used in probe evidence and is never passed as application
  message or placement input.

## 10. Smoke Run

1. Prepare a per-run Location Store, Relocation Store, and maintenance store. The two Framework
   stores may use the same Redis deployment, but their providers and key prefixes stay separate.
2. Start Ops and confirm control STREAM readiness.
3. Start ZoneNode A/B and confirm zone capability, mesh peer, and fanout readiness.
4. Start Gateway and confirm game STREAM readiness.
5. Run the headless self-check and the Chromium browser scenario.
6. Confirm graceful replacement, crash replacement, and the cross-owner pair probe as separate
   runs.
7. Check runtime evidence and the completion marker.
8. On both success and failure, clean up the resources and processes this run created.

```text
zoneworld=completed
```

The runner checks relocation, border sync, and Ops observe/announce/maintenance evidence together
with the completion marker above. Per-step markers are only used when the per-language runner
actually prints them, and aren't treated as the common document's message contract or topology
names.

## 11. Completion Criteria

- .NET, Node.js, and the shared TypeScript browser use the same JSON declarations and business
  semantics.
- The base topology expresses only the Client and server components and the STREAM, RouteMesh, and
  fanout connections.
- The Actor owns coordinate authority, and the Zone Spot only owns the copy and border snapshot.
- The movement rejection order, zone geometry, tick, sort, and expiry rules are the same across
  every language.
- A cross-node join preserves the same ActorId and ObjectGeneration and the kept session binding.
- A repeated relocation that returns the Actor to a previously visited node preserves the same
  identity and the bound session (the A→B→A round trip, ZW-B7).
- Message Follow's stated limits and terminal errors are confirmed by the self-check.
- Border sync uses only adjacent topics and doesn't publish to diagonal zones.
- Announce and maintenance use classic fanout, and node status uses runtime events and explicit
  reports.
- NodeId and the transport RID are distinguished, and the automatic routing ID gate passes.
- Bots move under the same PlayerActor rules with no bound session and produce no client push.
- Only the Framework public API and the typed JSON codec are used, with no raw frames, private
  routes, or custom owner selection added.
- A Ready owner failure is not shown as crash failover, and the Unavailable boundary is kept.
- The runner performs build, readiness, browser/headless self-check, evidence, and cleanup.

### 11.1 Scenario ID Families

The self-check scenario IDs (`ZW-*`) are grouped by intent. Each family covers one of the
criteria above; the runner's evidence names the individual IDs.

| Family | Intent |
| --- | --- |
| ZW-A | Movement basics: entry, in-zone movement, rejection order, visibility, ordering |
| ZW-B | Relocation and session: border sync, cross-node relocation, identity and binding continuity — including ZW-B7, the A→B→A round trip back to a previously visited node |
| ZW-C | Ops observation: node status, shutdown, disconnect, spot event reports |
| ZW-D | Fanout announce: one publish reaching every node's subscriber and zone spots |
| ZW-E | Maintenance: targeted enable/disable, entry refusal, restart persistence, diagnostics |
| ZW-F | Bots: unattended movement, population, no client push, reversal on rejection |
| ZW-G | Node identity and replacement: NodeId vs transport RID, routing ID gate, replacement |

### 11.2 Individual Scenario Definitions (canonical)

Every language runner implements the individual definitions below, and prints
`zoneworld=completed` only as the AND of the verdicts of all implemented IDs. Unless stated, the
precondition (P) is "all components ready in §10 order + a browser or headless client connected
to the Gateway".

| ID | Precondition | Action | Assertion |
| --- | --- | --- | --- |
| ZW-A1 | base | JoinWorldReq | JoinWorldRes = zone-nw,(25,25); replied after admission per the §7.1 norm |
| ZW-A2 | A1 | same-zone MoveMsg | updated coordinates and ZoneId in ZoneStateNotify |
| ZW-A3 | A1 | one out-of-range, one over-distance, one diagonal, one maintenance move | MoveRejectedNotify reasons in the fixed OutOfRange→TooFar→DiagonalCrossing→ZoneMaintenance order |
| ZW-A4 | two players in one zone | each moves | both players present in the same ZoneStateNotify |
| ZW-A5 | A4 | receive ZoneStateNotify | Players sorted ascending by PlayerId UTF-8 bytes, own-zone value preferred |
| ZW-B1 | player inside the border band | wait ticks | border snapshot arrives only at adjacent zones, never diagonal ones |
| ZW-B2 | cross-owner adjacent pair (capacity spread per §3, discovered by probe) | border-crossing MoveMsg | relocation completes, ZoneChangedNotify, subsequent notifies on the same WebSocket |
| ZW-B3 | right after B2 | ActorLocationProbe | same ActorId/ObjectGeneration, only owner generation advanced |
| ZW-B4 | B1 with publishing stopped | 3 ticks pass | that FromZoneId snapshot is removed (expiry) |
| ZW-B5 | B2 | one-way probe to the old owner route | processed exactly once at the committed target (Follow), no resubmission |
| ZW-B6 | B2 | request probe to the old owner route | operation id/generation/payload/reply route preserved, no source Store re-read or hidden retry |
| ZW-B7 | B2 | move back to the original owner (A→B→A) | same identity and binding kept |
| ZW-B8 | B2-capable state | runner delays/blocks the session route-update delivery beyond the seal timeout (injection), then a border move | the Framework closes the physical connection (WebSocket close observed); the client reconnects and performs JoinWorld again, rebinding to the existing Actor with the same PlayerId (§7.5) |
| ZW-C1 | base | Ops WatchNodesReq | Registered and Connected each accurate for both ZoneNodes |
| ZW-C2 | C1 | graceful ZoneNode shutdown | Connected=false observed (runtime event, not polling) |
| ZW-C3 | C2 | 15-second report TTL passes | Registered=false observed (§2.2 TTL rule) |
| ZW-C4 | base | zone tick timer fault injection (runner) | failure observed via spot event report, no zone stall |
| ZW-D1 | base | AnnounceWorldReq | AnnouncementId reaches every node/zone game client exactly once |
| ZW-D2 | D1 + third subscriber added | announce again | everyone including the new subscriber receives it (proves no hardcoded publisher list) |
| ZW-E1 | base | SetMaintenanceReq(node,true) | only that NodeId's desired state changes, recorded in the store |
| ZW-E2 | E1 | new join into a zone on the maintenance node | rejected ZoneMaintenance by target OnActorJoin (§7.4 sole decider) |
| ZW-E3 | E1 | movement inside a maintenance zone | allowed |
| ZW-E4 | E1 | move to a different zone on the maintenance node | rejected ZoneMaintenance (same-zone only) |
| ZW-E5 | E1 | ZoneNode restart | maintenance state restored for the same NodeId |
| ZW-E6 | base | NodeDiagnosticsReq | latest zone list, player counts, maintenance returned |
| ZW-F1 | base | observe bots | 8 bots move with the fixed §7.3 initial values/trajectories |
| ZW-F2 | F1 | X-bot reaches the boundary | cross-owner bot relocation completes (no binding) |
| ZW-F3 | F1 | induce a bot move rejection | direction reverses |
| ZW-F4 | F1 | observe client pushes | no push targeted at bots (negative evidence) |
| ZW-G1 | base | RID observation (probe) | actual `zn-<lowercase-uuid-v4>` format check (a printed marker alone is insufficient), distinct across nodes |
| ZW-G2 | base | varied start order | readiness and routing normal |
| ZW-G3 | base | graceful replacement (stop→start) | new RID with the same NodeId report, new objects normal |
| ZW-G4 | base | crash replacement (kill→start) | replacement per §7.5: previous operations keep the Unavailable boundary + the new process is normal |
| ZW-G5 | G3/G4 | routing ID gate | every §9.3 item |

A per-language runner may implement some IDs runner-driven (e.g. the C4 fault injection), but it
must not change an ID's precondition/action/assertion semantics. A new ID is added to this
document first before any runner introduces it.

#### Fixed values for the scenarios that stop and restart a ZoneNode

Several scenarios stop a ZoneNode. **How it is stopped, and which zone set it comes back with, is
not a per-language choice.** Leaving these values unstated is why four implementations used
different signals, so the same ID tested something different in each language.

| ID | Stop | Why |
| --- | --- | --- |
| ZW-B4 | abrupt | publishing has to cut out at once for the 3-tick expiry to be observable |
| ZW-C2 | graceful | the row's precondition is a normal shutdown; it observes the drain path's disconnect event |
| ZW-C3 | abrupt | §2.2 — "a crashed node cannot send a false report, so this TTL is the only false transition rule" |
| ZW-E5 | abrupt | shows the maintenance desired state lives in a store outside the process |
| ZW-G3 | graceful | the row's precondition is a normal replacement (stop→start) |
| ZW-G4 | abrupt | the row's precondition is a crash replacement (kill→start) |

**A restarted ZoneNode does not take its zones back.** §2.2 fixes that a Ready owner failure is
never an automatic replacement, so the restarted process comes up with the **replacement
configuration that reaches ready with zero zones** — the same whether the stop was graceful or
abrupt. Only the initial cold start claims zones.

Leaving this unstated makes a restarted node demand two zones and retry the claim until its budget
runs out, which is exactly the state the cpp implementation was in.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=Math.max(d.body?d.body.scrollHeight:0,d.documentElement?d.documentElement.scrollHeight:0);if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
