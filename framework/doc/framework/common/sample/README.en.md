# Framework Common Sample Scenarios

This directory defines the sample scenarios shared by every framework language. A per-language
sample may differ in implementation approach and syntax, but it matches this document's server
roles, message flow, message fields, and verification criteria.

The common sample is the authoritative source for actual business flow, but it's not the basis for
a new public API contract. Public behavior and constraints are owned by the [common spec](../README.en.md),
and this document shows that contract as an executable business flow. A per-language sample doesn't
copy one language's implementation as the standard — it follows this common scenario together with
that language's spec.

Bingo, TicTacToe, SupportChat, DeliveryDispatch, ShoppingMall, and GameQuest are provided in common
by five framework languages (.NET, Java, Kotlin, Node.js, C++). ZoneWorld is provided by .NET and
Node.js, sharing a TypeScript browser client. The supported languages follow the same role
separation, request/response/notify names, state fields, and smoke verification order. Even if the
per-language API representation differs, the same framework features must be confirmable in the same
order.

A per-language guide doesn't redefine the common sample's purpose, server composition, message
contract, state transitions, or verification order. Copying this content per language could create
implementation standards that diverge from the common document. Whether a per-language DTO
representation needs to be written separately is judged per the
[per-language representation criteria](languages/README.en.md). If there's no actual representation
difference, no per-language sample document is created.

## Sample List

| Sample | Purpose | Server Composition | Connection Method | Handler Registration Method | Default Payload Codec |
|------|------|-----------|-----------|-------------------|--------------------|
| [Bingo](bingo/README.en.md) | Shows a level-based matchmaking Instance Spot, session gateway, actor binding, room User Spot, timer, and bound push in one flow. | `Session`, `Api`, `Matchmaking`, `Play` separated | Location-store-based automatic connection | **automatic registration** | Protobuf |
| [TicTacToe](tictactoe/README.en.md) | Shows manual endpoint scale-out with 2 APIs and 2 Plays, room routing based on the official Redis Location Store, and a realtime game flow. | 2 `Api`, 2 `Play`, `Play` owns the stream session together, with no separate `Session` server | **manual endpoint connection** | **automatic registration** | JSON |
| [SupportChat](supportchat/README.en.md) | A customer and an agent talk in the same conversation Spot, confirming reconnect, idle timer, close, and bound push. | `Session`, `Api`, `Support` separated | Location-store-based automatic connection | **automatic registration** | JSON |
| [DeliveryDispatch](deliverydispatch/README.en.md) | Confirms delivery assignment, timeout reassignment, status push, and customer stream push. | `Dispatch`, `CourierSession`, 2 `CourierMeshNode`, `Tracking`, `CustomerGateway` separated | Location-store-based automatic connection | **automatic registration** | JSON |
| [ShoppingMall](event/shoppingmall.en.md) | Separates `CommerceApi` (the HTTP edge) from `OrderWorkflow` (the order owner) to build event-sourced order processing and a lookup model. | `CommerceApi`, `OrderWorkflow` separated | Location-store-based automatic connection | **automatic registration** | JSON |
| [GameQuest](event/gamequest.en.md) | Gathers gameplay events into a per-player owner spot to update an event-sourced quest aggregate and lookup model. | `Session Server`, `PlayerQuestSpot` owner distributed across MeshNodes | Location-store-based automatic connection | **automatic registration** | JSON |
| [ZoneWorld](zoneworld/README.en.md) | Shows, via a browser UI, a zone-partitioned MMORPG's boundary crossing (actor relocation), boundary sync, bots (actors with no bound session), and the operations console managing it (runtime events, fanout announcements, node targeting). | `Gateway`, 2 `ZoneNode`, `Ops` separated | Location-store-based automatic connection | **automatic registration** | JSON |

> ZoneWorld confirms zone movement and node operations via a browser UI. The .NET and Node.js
> servers share one TypeScript client with the same wire contract. It verifies `ws`/`wss`,
> request/reply, push, reconnect, and explicit flow propagation in an actual Chromium.

## Channel Roles And The Physical Topology Standard

A Channel send/request specifies its target with a single ChannelName. The samples don't add an
application helper that hides the MeshName — they directly use each language's formal Channel
client. The choice between RouteMesh and ClientServer is decided not by a single call, but by the
overall business direction between two process roles and whether stateful addressed messaging is
needed.

- A role that needs Spot/Actor direct messaging, actor relocation, or Logical Multicast uses
  RouteMesh.
- If two roles each independently start business sends/requests, one RouteMesh peer connection is
  used bidirectionally. A ClientServer Channel isn't created per call direction, duplicating the
  connection.
- ClientServer Channel is used when only one side starts business calls and there's no RouteMesh the
  two roles would share.
- `Client()` registers only the send path, and only `Server()` provides the handler and weight.
  `SetWeight(0)` isn't used to represent a client role, and a fake ChannelName membership isn't
  added.
- MeshNodes that registered an object role only as `Client()` don't connect to each other. Even
  registering a manual endpoint doesn't bypass this restriction. Connecting clients that have no
  reason to call each other would hide a configuration error while keeping an unnecessary socket
  open.
- If an Object Client process must also provide a channel Server, that channel is registered as an
  independent ClientServer topology. A channel Server role isn't mixed into the Object Client
  RouteMesh.
- A local sample runner uses the default BindHost `127.0.0.1` and automatic port 0. A
  Container/Kubernetes deployment specifies, in `ConfigureNetwork()`, the AdvertiseHost that a remote
  peer connects to from the Pod or Service. A wildcard BindHost isn't recorded as the descriptor's
  advertised endpoint.
- A sample doesn't pre-share a specific MeshNode's `NodeRid` as a setting value, message field, or
  business constant. Actor/Spot creation and direct messaging use the global `ActorId`/`SpotId` and
  Location-Store-based placement, with the caller never choosing the owner node or passing the owner
  `NodeRid`. Manual topology also configures only the peer endpoint, and doesn't use the other
  side's `NodeRid` as an application route.
- Node direct messaging is used only for Admin/Ops management features. Even then, only the
  `NodeRid` currently discovered from the runtime descriptor can be used as the management target or
  a display value at that moment. It must not be stored and used as a fixed target for the next run,
  or used to infer Actor/Spot location. A sample that needs a business `NodeId` defines it as a
  domain identifier distinct from the transport `NodeRid`, and doesn't use it for framework routing.
- STREAM and classic Pub/Sub aren't Channel egress, so they keep an independent listener.

A per-language sample topology regression must read the following common fixture. The same expected
values aren't copied into per-language source.

```text
framework/doc/framework/common/sample/fixtures/
`-- channel-topology.json
```

The fixture's `channelKinds` distinguishes whether each `channels` entry uses a physical RouteMesh
connection or a separate ClientServer socket. Whether an Object Client pair needs a connection is
judged using only `RouteMesh` Server membership. `ClientServer` Server isn't included in this
judgment.

### Physical Connections Per Sample

| Sample | RouteMesh Scope | ClientServer Scope | Separate Connections |
|---|---|---|---|
| Bingo | Api/Matchmaking use `bingo.matchmaking`; Session/Api/Play use `bingo.play`. The two Meshes don't share an object provider or placement pool. Object Clients with no RouteMesh Channel Server don't connect to each other. | `bingo.api`: Session/Play Client → Api Server | Session STREAM, Redis matchmaking state |
| TicTacToe | The Api Object Client and the two Play Object Servers manually connect on `tictactoe`. Both Plays also provide milestone-multicast Server membership. | `tictactoe.api`: Play Client → Api Server | Play STREAM, Redis location store |
| SupportChat | Session/Api are an Object Client, and Support is an Object Server, sharing `supportchat`. Since the two Object Clients have no RouteMesh Channel Server, they don't connect to each other. | `supportchat.api`: Session/Support Client → Api Server | Session STREAM |
| DeliveryDispatch | Dispatch/CourierSession are an Object Client, and CourierActorNode is an Object Server, sharing `deliverydispatch.courier`. Tracking is an Object Client, and CustomerGateway is an Object Server, sharing `deliverydispatch.customer`. | `deliverydispatch.dispatch`: CourierActorNode Client → Dispatch Server; `deliverydispatch.tracking`: Dispatch Client → Tracking Server | Courier STREAM, Customer STREAM |
| ShoppingMall | CommerceApi/OrderWorkflow share one `shoppingmall.workflow`, with only OrderWorkflow providing an Instance factory | none | Commerce HTTP, shared event/projection store |
| GameQuest | GameApi/QuestMission share one `gamequest` | none | GameApi STREAM, shared state store |
| ZoneWorld | Gateway/ZoneNode/Ops share one `zoneworld.mesh` | none | Gateway STREAM, Ops STREAM, `zoneworld.broadcast` classic fanout |

### Channel Roles

| Sample | Client Role | Server Role |
|---|---|---|
| Bingo | Independent ClientServer's Session/Play: `bingo.api` | Independent ClientServer's Api: `bingo.api`; `bingo.play` RouteMesh's Play: `bingo.room` Logical Multicast membership |
| TicTacToe | Independent ClientServer's Play: `tictactoe.api` | Independent ClientServer's Api: `tictactoe.api`; the two RouteMesh Plays: `tictactoe.player.milestone.channel` Logical Multicast membership |
| SupportChat | Independent ClientServer's Session/Support: `supportchat.api` | Independent ClientServer's Api: `supportchat.api` |
| DeliveryDispatch | Independent ClientServer's CourierActorNode: `deliverydispatch.dispatch`; Dispatch: `deliverydispatch.tracking` | Independent ClientServer's Dispatch: `deliverydispatch.dispatch`; Tracking: `deliverydispatch.tracking` |
| ShoppingMall | none. CommerceApi starts a direct Spot call with the Order ID's global SpotId, specifying an `InstanceSpot("shoppingmall.order-workflow")` marker | none. OrderWorkflow provides only the `OrderWorkflowSpot` Instance factory |
| GameQuest | none. GameApi starts a direct Spot call with the Player ID's global SpotId, specifying an `InstanceSpot("gamequest.player-quest")` marker | none. QuestMission provides only the `PlayerQuestSpot` Instance factory |
| ZoneWorld | Gateway: actors; ZoneNode: report | actor-creating ZoneNode: actors; Ops: report; only ZoneNode registers as a zone multicast target |

TicTacToe's manual RouteMesh initiator is fixed as API-A→Play-A/Play-B, API-B→Play-A/Play-B,
Play-A→Play-B. Object Clients API-A and API-B don't connect to each other. A Play→API request uses
a separate ClientServer `tictactoe.api` connection. A RouteMesh connection isn't reused as a
reverse-direction business channel.

## Actor/Spot Creation And Matchmaking

Samples don't relay creation requests through a channel handler. The process wanting to create
directly calls the public manager, letting the Framework select the owner node using the Location
Store's candidates and weight.

| Purpose | Call | Identifier Owner |
|---|---|---|
| Create only a new Actor with an ActorId the application decided | Actor manager's `Create` | application |
| Get an existing Actor or create it exactly once with an ActorId the application decided | Actor manager's `GetOrCreate` | application |
| Create a new User Spot with the Framework issuing the ID | Spot manager's `Create` | Framework |
| Create exactly once an already-decided SpotId, like a room code/zone code/match reservation | Spot manager's `GetOrCreate` | application |
| Create on demand by sending the first message to an Instance Spot | The `InstanceSpot` marker on a Spot send/request | the SpotId the caller specified |

`Create` and `GetOrCreate` can specify `InMesh` when needed for the initial placement Mesh. A direct
message sent to an already-existing Actor/User Spot doesn't attach `InMesh` — the Framework finds
the current owner by global ID. A send/request that first activates a missing Instance Spot can use
`InMesh` together with `InstanceSpot`.

TicTacToe's `CreateGame` is a request to create a new room. The API uses the SpotId
`SpotManager.Create` returns as the RoomId. It doesn't choose a specific Play endpoint or NodeRid as
the owner.

Bingo uses two RouteMeshes since matchmaking and actual gameplay execution have different resource/
lifecycle/placement pools. The API process registers an Object Client MeshNode on each Mesh.

| MeshName | Object Server | Feature Provided |
|---|---|---|
| `bingo.matchmaking` | Matchmaking | A per-level-bucket `bingo.matchmaker` Instance Spot |
| `bingo.play` | Play | The Bingo room User Spot and player Actors |

The sample runner starts one Matchmaking process, but this doesn't create a singleton contract. An
operator can run multiple Matchmaking nodes, and the Framework distributes the per-level Instance
Spots across eligible nodes. A Matchmaking node doesn't register a room User Spot or player Actor
type, and a Play node doesn't register the matchmaker Instance Spot type.

The API converts the player level into a bounded bucket and sends a request to the
`bingo-matchmaker-level-<bucket>` SpotId. If the Spot is missing, it's automatically created with an
`InstanceSpot("bingo.matchmaker")` and `InMesh("bingo.matchmaking")` intent. The Instance Spot
processes requests of the same bucket turn by turn, selecting or newly creating an open room
reservation in Redis. Redis owns the actual state, and the Instance Spot uses the
`RecreateOnRelocation` policy. When the idle timer expires, the Spot internally calls
`Context.CloseAsync()`.

The Redis match reservation owns the stable RoomId and the same RoomSettings. The first request and
any concurrently arriving requests all receive the same reservation, and the API calls
`SpotManager.GetOrCreate` with `InMesh("bingo.play")` for all of them, waiting until the room Spot
becomes Ready. Only the first request should wait for creation completion — the rest must not
immediately return the RoomId. If creation fails, an unsuccessful RoomId isn't returned to the
participant. The responsibility of waiting until the participant count fills up belongs to the
created room User Spot, not the matchmaking request.

## Verifying Relocation And Message Follow

A sample demonstrating relocation measures the service interruption time of a single unit the
application uses, not the host's overall elapsed time. The measurement window is from when the
source blocks new work to when the target sends an ACK that it has started accepting new work. A
single Actor, a single Instance Spot, and a single SpotWide User Spot aggregate each target a
default of within 1 second. Exceeding 1 second doesn't cancel or roll back the relocation. The
detailed standard follows [Graceful Drain And Handoff §7.1](../spec/28-graceful-drain-handoff.en.md#71-service-interruption-time-target-per-relocation-unit).

A SpotWide User Spot moves Spot state and member Actor state as a single relocation unit. Multiple
Actor payloads aren't stored to or read from the Relocation Store sequentially — they're processed
in parallel within a configured I/O concurrency and in-flight byte limit. The queue, accepted
journal, and logical timer are stored by the Framework and restored at the target, keeping their
order.

Entry Spot and PerActor User Spot don't move Spot application state. The Framework prepares a
stateless shell of the same SpotId at the target and moves Actors one at a time. The Application
manages only the Actor adapter's state, and the Spot doesn't restore a list of ActorRefs.

A sample where the SpotWide factory chose the `ApplicationSignaled` boundary registers
`RelocationReady().Defer()` during a safe business turn. The Framework calls
`OnRelocationReadyCompletedAsync` regardless of whether relocation actually occurs. The Application
starts the next round only after this callback finishes. An example that calls `Defer()` on the
default `AnyTurnBoundary` isn't built.

[Message Follow](../spec/21-location-runtime.en.md#63-delivering-a-message-arriving-at-a-previous-owner-to-the-new-owner)
is a feature that delivers an Actor/Spot message arriving at the previous node right after an owner
change to the current owner. The sample's verification confirms each of the following cases.

- A one-way send and a request/reply started during relocation reach the target owner. Whether the
  business effect duplicates is confirmed via handler idempotency verification using the preserved
  operation ID.
- The reply route, operation ID, payload, and ObjectGeneration are preserved even after relay.
- The source uses only the target it recorded when relocation completed, without re-querying the
  Location Store during Message Follow.
- The `MessageFollowDuration` default is 30 seconds, and 0 means relay isn't used. If a sample sets
  a shorter value, it leaves the actual configured value and expiry time as evidence.
- Relay works only up to 8 hops, and within 1,024 messages and 16 MiB per move. Confirm each of:
  route-missing/period-expired/loop/hop-exceeded is `Unavailable`, generation mismatch is
  `InvalidOperation`, and message/byte limit exceeded is `CapacityExceeded`.
- A failure whose execution status is ambiguous after a relay failure or target admission isn't
  automatically resubmitted to a fresh owner. A failed operation ends in a terminal, and only the
  next call resolves fresh.
- A new call started after the Message Follow period ends finds the current owner from the Location
  Store, without relying on the previous source relay.

## Message Naming Principle

A sample message name must reveal the framework call method before the domain event name. Even for
the same business flow, whether it's request/reply, a one-way send, or a client push changes both
the value the caller waits on and the handler contract. Per-language samples and e2e use the suffixes
below with the same meaning.

| Call Method | Suffix | Basis |
|-----------|--------|------|
| request/reply | `Req` / `Res` | A call that waits for a response, like `Request(...)`, `RequestToChannel(...)`, a route request, a stream request, or an HTTP request |
| send | `Msg` | A one-way message delivered with no response, like `Send(...)` |
| client push | `Notify` | A notification the server pushes to the client via stream/session, which the client waits to receive |
| publish (pub/sub · fanout) | `Event` | A message where **the publisher doesn't know the receiver**, like a Spot context's `Publish(...)` or a classic fanout publish. Logical Multicast and channel fanout fall here |

A message called as a request is named with the `Req`/`Res` pair, even if its business name looks
like `Changed`, `Accepted`, `Created`. For example, a flow that requests a state change and waits for
an ack should correctly be `DeliveryStatusChangedReq` and `DeliveryStatusChangedRes`. Conversely, a
flow where the server pushes a state change to a customer client uses `Notify`, like
`DeliveryStatusNotify`.

Suffixes like `Command`, `Result`, `Ack` are not newly added as a sample's wire message name. These
names can make internal domain events, business commands, processing results, and transport
responses look mixed together. Even when tidying up an existing sample message, organize it into one
of the suffixes in the table above, based on the call method. `Event` is used **only for publish
calls** — it's not attached to a message sent by request or send.

This rule applies to a ZLink wire message that actually crosses a stream, channel, actor, or Spot
boundary. The following two cases are exceptions, since they aren't wire messages.

- **Domain event stream (SoR) records**: A domain event appended to the event store, like
  `OrderStartedEvent`, `QuestProgressed` in the event-sourcing samples (ShoppingMall, GameQuest), is
  not the target of this rule. This name isn't itself a transmitted packet — it's a record piled up
  in a durable store, and since event-sourcing vocabulary is itself the domain representation, the
  `Event` suffix is neither forced nor forbidden — the domain decides its own natural name
  (`OrderStartedEvent`, `QuestProgressed`).
- **In-process domain/application port contracts**: A port DTO (e.g., `ReserveInventoryCommand`)
  that calls a domain module within the same server process is a language-neutral contract that
  isn't dispatched over ZLink, so it can keep the `Command`/`Result` suffixes.

Conversely, an internal message actually delivered from an entry-spot to an owner spot via a real
`SendToSpot`/`RequestToSpot` is not an exception. Such a message is named `Msg` (one-way send) or
`Req`/`Res` (request/reply), matching its call method.

## The Spot Execution Turn And Terminator Sample Standard

**Every sample picks its three terminators by the same standard**
([04 §1.1](../spec/05-async-execution-policy.en.md)).

| Terminator | Execution Line | When |
|---|---|---|
| `submit` | continues right away (one-way) | doesn't write a response |
| **`async`** (default) | **keeps the turn** | judges/changes **this spot's state** with the wait result. handler = one turn |
| **`yield`** (opt-in) | **gives back the turn** | the wait is **unrelated to this spot's shared state**. That wait must not stop the whole spot |

There's one criterion — **is this wait related to this spot's shared state?** `yield` isn't a
convenience — it's a tool to **pull out only the unrelated wait, while keeping serial execution's
benefit.** So the default is `async`, and `yield` is used only when there's a reason.

Wherever `yield` is used, **don't keep judging the same mutable state continuously across the
`yield`.** Since another message can be processed first while yielding, re-check the needed state
after resuming.

The samples' standard usage sites are as follows.

| Sample | Location | Terminator |
|------|------|-----------|
| [Bingo](bingo/README.en.md) §7.1 | The room Spot's actor join/leave queries and records the player's record on the Api server | **`yield`** |
| [Bingo](bingo/README.en.md) §3.1 | The Matchmaker Instance Spot decides the same bucket's next state from the Redis reservation result | `async` |
| [DeliveryDispatch](deliverydispatch/README.en.md) §6.1 | The Entry Spot initializes the application state of a newly delivered Actor | `async` |
| TicTacToe | Game join leads directly into the game-state flow | `async` |

**Workers and the HTTP client are the same axis** ([04 §1.2](../spec/05-async-execution-policy.en.md),
[12 §3](../spec/http-client/12-http-client.en.md)). An external HTTP/legacy API uses the HTTP
client's terminator directly, and an asynchronous wait with no terminator of its own, like a DB
driver or an external SDK, is wrapped with `RunIoWorker(...)`. CPU work is handed off to
`RunCpuWorker(...)`.

A sample that only uses the decoded body from an HTTP response directly receives the DTO with the
per-language `Fetch`/`fetch` terminal. It uses the `Async`/`submit`/`await` terminal, which returns
a typed response envelope, only when status or headers must be verified. Code that immediately pulls
out the response's `.Body`/`.body()` just to get the DTO isn't placed in a sample.

## Sample Porting Standard

Bingo and TicTacToe are exception samples that each showcase a feature they're responsible for.
Bingo shows a Protobuf payload, matchmaking/gameplay placement pools split across two RouteMeshes,
and a location-store-based gateway; TicTacToe shows a scale-out flow using manual endpoints together
with the official Location Store.

The other canonical samples (SupportChat, DeliveryDispatch, ShoppingMall, GameQuest) follow the
standard below.

- The payload codec defaults to JSON. This is so payloads are easy to compare across samples, and so
  event-sourcing and projection state stay human-readable.
- A sample needing Protobuf or MessagePack installs the framework codec extension package and
  registers the extension during configuration. The sample's DTO, handler, and client call shape
  don't change because of the codec.
- A server-to-server connection is configured as a shared location-store-based automatic connection.
  This is so the sample code doesn't directly manage endpoint connection order or route warmup.
- A process registers only one RouteMesh unless there's a special physical isolation requirement,
  splitting per-business routes across multiple ChannelName memberships of that MeshNode. The Bingo
  API is an exception. Since the Matchmaking Instance Spot and gameplay User Spot/Actor must be
  placed on different providers/resource pools, it registers both `bingo.matchmaking` and
  `bingo.play` Object Clients together. Classic pub/sub and STREAM nodes aren't a RouteMesh
  ChannelName, so they each keep an independent registration. To add a RouteMesh in a sample, first
  record in that sample document the reason the physical mesh needs to be separated and the
  connection boundary the user perceives.
- **Absolute rule: only TicTacToe may use a manual connection.** Every sample other than TicTacToe
  must never add or keep a manual connection, for any reason. Build/run success, a temporary
  automatic-connection failure, debugging convenience, and per-language implementation differences
  are not exception grounds. Other samples must not pass a peer endpoint directly to a ChannelName
  client, directly connect a Spot router/pub-sub peer, or bypass a server-to-server call with a
  fixed HTTP endpoint. That is, application code doesn't use a manual peer connection, a
  Spot-dedicated router/pub-sub peer connection, or a peer-targeted `ZLinkHttpClient.Create(...)`
  in server code. If automatic connection fails, don't add a manual connection to the sample — fix
  the framework implementation where location-store registration/lookup/connection lifecycle is
  broken. This prohibition applies across the entire per-language sample, and even one violation
  means that sample's change isn't judged complete.
- **Automatic registration is the default.** In a language where the framework can scan and
  register handlers, handlers are automatically registered with no separate registration call
  ([05 §8](../spec/06-framework-api.en.md#8-handler-registration-and-dispatch)). Repeating the
  handler list in every sample would make the public usage example verbose, and a client scenario
  would catch a missing handler addition too late.
- **Manual topology and handler registration are separate matters.** TicTacToe also declares
  handlers with annotations/attributes/decorators and automatically registers them via
  assembly/module scanning. It doesn't repeat the handler list in configuration code just to
  showcase manual endpoint connection.
- Since C++ doesn't use a runtime reflection scanner, it explicitly registers handlers with
  compile-time types. The exact surface follows the
  [C++ Handler Public Contract](../spec/server/languages/cpp/interfaces/03-channel-messaging.en.md).
  Only the registration method differs — the message, role, codec, and verification criteria don't
  change.

## The Dispatch Error Log Standard

Every per-language sample must leave framework message dispatch errors in the sample log. A message
that can't be handled at the dispatch stage — an unregistered request, a payload decode failure, a
handler exception — must be immediately confirmable during a sample run.

Each sample configures Framework diagnostics in `AddZLinkFramework(...)` and records it with its own
logger. Different samples, like Bingo, TicTacToe, SupportChat, don't share a logging setup helper.
Samples must be independently readable and portable, and this standard breaks if the logging setup
depends on another sample's directory.

Log output doesn't build a new logging system — it follows the logger each sample already uses. A
sample that already writes a file log directly records to that file logger, and a sample whose run
script saves stdout/stderr to `logs/*.log` records to that sample's console logger. The trace
attributes follow the exact snake_case names and inclusion conditions of
[Message Flow Tracing](../spec/26-message-flow-tracing.en.md). `surface`, `message_kind`, `outcome`
are included in the message-flow record, and `reason` is included when there's a cause, and
`action` for a dispatch error. `packet_name` is recorded only when a typed packet name exists, and
`correlation_id` only when connecting a request and its terminal reply. For the Channel path, only
`channel_name` is kept; for the Spot path, `spot_id`; for the Actor path, `actor_id` — only the
identifier matching the actual logical target.

An implementation providing structured logs uses the same spec's fallback keys — `event`, `kind`,
`channel`, `packet`, `spot`, `actor`, `corr`, etc. — as-is. The two schemas aren't renamed to
camelCase, and a field with no value isn't forced to an empty string.

A sample sets the diagnostics level and sampling in each server process's `AddZLinkFramework(...)`
configuration. The Framework delivers the record to the standard tracing and structured-logging
provider, and the application doesn't implement a separate observer/event DTO or a
Framework-specific log file. `run_sample.sh` and `run_sample.ps1` save process output to
`logs/*.log`, so a message-flow error is also confirmed in the same sample log.

A sample handler doesn't re-handle, in smaller pieces inside the handler, a dispatch error the
framework already handles. It doesn't catch an exception inside a request, actor-request, or
session-packet handler just to log it and rethrow, or turn a domain exception into an arbitrary
`error` field response. Such an exception is left to the Framework dispatch boundary to handle via an
error reply, drop, and standard diagnostics recording. A sample handler must focus on showing the
success path and domain behavior — code that turns a failure into a normal business response is
included only when that message contract explicitly defines such a failure state.

## The Sample Configuration Delivery Standard

Every language's sample must follow the
[Sample/E2E Configuration Policy](../sample-e2e-configuration-policy.en.md). The runner builds a
per-run role configuration file, and hands the framework host only the configuration file path. A
standalone client that isn't a Framework host receives the endpoint it directly connects to, the
request timeout, and the scenario selector as explicit CLI options, validated once at startup.
Endpoint, Redis, routing id, timeout, and log path aren't delivered via environment variables or JVM
system properties, and the number of environment variables the server and client application code
can use directly is 0.

An environment-variable interface isn't provided together as a compatibility path. A Framework host
uses a configuration file and typed binding; a standalone client uses validated CLI input, or a typed
configuration file when needed.

## The Sample Run Script And Redis Isolation Standard

Every language's sample runner must have the same usage meaning and the same Redis launch method. A
per-language implementation can use different tools — shell, PowerShell, npm, Gradle, dotnet, CMake —
but matches the execution contract below.

**Mandatory isolation rule:** each sample run that needs Redis must create a fresh, dedicated Docker
Redis container used only by that run. An already-running container, host Redis, or a Redis endpoint
created by a different sample or E2E must not be shared or used as a fallback. Specifying only a
different key prefix doesn't permit instance sharing either. This rule's purpose is to make sure
cleanup timing and stored data don't affect a different run.

The standard templates are placed under this directory's `runner-templates/`.

- `runner-templates/redis-common.template.sh`: the Redis helper standard
- `runner-templates/run_sample.template.sh`: the individual sample runner standard
- `runner-templates/run_samples.template.sh`: the integrated sample runner standard

- An individual `run_sample.*` is responsible for the order: build → create log directory →
  prepare Redis if needed → start servers → confirm readiness → run the client self-check → clean
  up servers and Redis.
- Each language has a Redis helper shared by its sample runners. The helper provides, as common
  functions, per-run Redis container start and cleanup of the container id that run created, so an
  individual sample script doesn't assemble Docker commands directly.
- For a sample that needs Redis, the runner directly starts a dedicated Docker Redis container per
  run. It must not reuse an already-running Redis container or a host Redis endpoint. Even with a
  different Redis key prefix, mixed cleanup, fault injection, latency injection, and inter-sample
  data-cleanup timing can cause test interference. The sample application code must not call Docker
  or own the Redis container lifecycle.
- If a Docker Redis can't be created, the runner fails immediately. It must not auto-fall-back to
  host Redis or a different run's endpoint and treat that as success.
- Starting the Redis container uses the same order in every language: create the container with
  `docker create --name <scoped-name> --tmpfs /data -p 127.0.0.1::6379 <pinned-redis-image>`, start
  it with `docker start <container-id>`, then read the running state and assigned host port with
  `docker inspect`. Relying on `docker run -d`'s output to handle the container id and port at the
  same time isn't used.
- Since sample Redis data is needed only during the run, a Docker volume isn't created. The `/data`
  volume the Redis image declares is overridden with `--tmpfs /data`, and container cleanup uses
  `docker rm -fv`. This keeps an anonymous volume from being left behind after repeated runs.
- When an individual `run_sample.*` starts Redis, it adds a prefix to the container name that
  reveals the language and sample run scope. For example, a Java sample should be clearly
  identifiable at a glance as the same language/sample scope, like `zlink-redis-java-sample...`, and
  a Kotlin sample as `zlink-redis-kotlin-sample...`.
- An individual `run_sample.*` and the integrated sample runner don't delete a different container
  with the same prefix at startup. Since the same-language runner can also run concurrently, prefix
  cleanup could remove a different run's dedicated Redis.
- An individual `run_sample.*` cleans up only the Redis container id it created itself, on both
  normal and failure exit. Broad prefix-based cleanup isn't put into an individual script's exit
  trap.
- The integrated sample runner doesn't clean up a different run's Redis — it sequentially calls each
  individual `run_sample.*`. This runner also doesn't run samples in parallel within one run.
- The integrated sample runner must be able to run only a specific sample list. With no argument, it
  runs every sample; with an argument, it sequentially runs only the specified sample runners. E.g.:
  `./run_samples.sh Bingo SupportChat`, or for a runner that needs to distinguish per-language paths,
  `./run_samples.sh java/Bingo kotlin/SupportChat`. The integrated runner doesn't re-implement a
  sample's internal procedure — it only calls the selected individual `run_sample.*`.
- The integrated sample runner doesn't re-implement per-sample internal behavior. It calls the
  selected individual `run_sample.*` once and manages only the final result. Redis endpoint
  creation, readiness, log location, and self-check detailed procedure are handled by the
  individual script and common helper.
- The Redis host port isn't fixed — Docker is allowed to assign a free loopback port. The runner
  reads the assigned port via inspect and delivers it to the application configuration. The Redis
  key prefix is also made unique per run.
- Even if a different sample/e2e is using Redis on the same host, that endpoint isn't borrowed. A
  new Docker Redis container must be created, using a different loopback port Docker assigned, to
  prevent test interference.
- Since Redis container creation can take a while, a short timeout is set on the Docker command
  itself, and actual Redis readiness is confirmed with a separate port/readiness wait function. A
  Docker command not responding and Redis not yet being ready aren't hidden behind the same sleep.
- If the Redis helper fails, the individual runner must also fail immediately. A shell runner
  doesn't receive the container id by reading a process-substitution result like
  `read ... < <(redis_start_function)`. This approach can let `read` itself succeed even if the
  helper failed, leading to the wrong outcome of starting the server without Redis. The helper is
  provided as a function that assigns a value to the caller's variable, like
  `zlink_redis_start_scoped_assign`, so a helper failure directly becomes a runner failure.
- The integrated sample runner doesn't retry an individual sample failure, including a bind
  failure. Even if the per-run port was reserved in advance, if the bind still fails, it must fail
  immediately instead of hiding it by repeating the same run, so the cause can be confirmed.
- On failure, the runner prints `log_dir=...` or the per-sample log location, leaving each process's
  stdout/stderr and the framework log in `logs/*.log`.

## Common Writing Principles

- A sample must show what the framework does on your behalf.
- Keep the domain rules small, and let the session, actor, Spot, channel, timer, and push flow be
  clearly visible in the code.
- Sample application code uses only the package entrypoint, DI token, builder, and client interface
  each language's framework publicly exposes. It doesn't directly import or reflectively access a
  maintenance-only implementation location like `internal`, `runtime`, `dist/runtime`. If a needed
  feature isn't in the public contract, the sample doesn't work around it — the framework's public
  contract is completed first.
- In this document, `Spot` means a stateful coordination point with an independent lifecycle. A Spot
  represents a unit where state and events gather, like a room, conversation, workflow instance, or
  player quest. A Spot can accept actor participation, but an actor isn't required. A Spot can
  process a directed request, publish an event, run a timer, or react to a pub/sub event.
- A server that owns realtime state implements it split into `Domain`, `Application`,
  `Infrastructure` responsibilities. The names below are a recommended structure, and the directory
  name can be changed to fit language convention, but the same responsibility split and dependency
  direction must be kept.
  - `Domain` handles pure domain rules, state transitions, result judgment, and domain event
    creation. It doesn't depend on framework types, sockets, streams, handlers, loggers, or a DI
    container.
  - `Application` handles use cases that use the domain, like room creation and room assignment. It
    provides a small, clear entry point a framework adapter can call.
  - `Infrastructure` handles the framework and external connections. ZLink actor, session, Spot,
    handler, notification publisher, and channel request handler go in this layer.
- A per-language sample uses the same roles and message names. If a language idiom requires a
  different name, the difference is recorded in the common document first.
- The flow where the client actually connects to the real server and confirms the request, push, and
  final state is named `ClientScenario`. For example, like `BingoClientScenario`, the sample name and
  the client scenario role must both be evident together. `TestScenario` can be mistaken for a
  separate test fixture, so it's not used as the name of a sample client's execution flow.
- The common document's message contract is read as a language-neutral schema. A per-language sample
  implements the same fields and meaning with a representation fitting its language, like a record,
  class, struct, interface, or type alias.
- **enum/status values are serialized on the wire as named strings, not integer ordinals.** A field
  that defines named values (`Assigned`, `Delivered`, `Won`, `Draw`, etc.) carries that exact name on
  the wire. If a language's enum type serializes to an integer by default (e.g., a C# enum with no
  string converter registered), **that language must explicitly register string encoding.** An
  integer ordinal **silently breaks consumers in a different language** when a value is inserted in
  the middle and shifts the order, and it can't even be read from a log. This rule applies to every
  sample, every language.
- **A nullable field either omits the key on the wire or carries `null` when it has no value, but
  that handling must match across languages.** If one language carries `null` while a different
  language's decoder only handles "key absent," it breaks cross-language. Only a field the document
  marks `Field?` is nullable — a field with no such mark is always carried.
- A wire message crossing a channel, route, stream, actor, or Spot boundary is kept as a named
  contract. Even in a language where a dynamic object is easy to build, like a Python `dict` or a
  Node.js object literal, `{ ... }` isn't written directly at the call site, and packet name strings
  aren't scattered around. A request, response, or notification payload is kept as a message type or
  schema in a shared contract location like `Shared/Contracts`, and the client and server use only
  that object's public interface.
- A message object's contract must not be hidden behind a per-codec convenience wrapper or a
  sample-specific helper. Regardless of whether JSON, MessagePack, or Protobuf is used, the sample
  code must directly use the public interface the connector and message object provide. A
  connector-specific codec package or a bindings codec package isn't guided as the sample's standard
  usage.
- An inline object literal is used only for a value that isn't a wire contract, like local state used
  within one function, a test helper value, or a parsing result. A sample prioritizes visibility
  that lets the same message flow be compared across languages, over being a short demo.
- Bingo and TicTacToe don't repeat the same feature. Bingo covers the gateway structure using
  matchmaking/gameplay placement pools split across two RouteMeshes and a shared Location Store;
  TicTacToe covers the scale-out structure using manual endpoints together with the official
  Location Store.
- The codec choice is kept simple so it doesn't get in the way of the sample's role. Bingo takes on a
  Protobuf payload with a schema clearly shared across languages, while TicTacToe and the rest of the
  samples default to an easy-to-read-and-compare JSON payload. Bingo's Protobuf usage must also show
  up only as a dependency and framework codec extension registration difference, not a business API
  difference.
- A domain identifier is named so its meaning is evident. For example, the value a client receives
  in TicTacToe is the Framework-issued string `RoomId`, not a Node RID. When a domain identifier like
  RoomId/ConversationId is itself the Spot address, that UTF-8 string is used directly as the SpotId.
  The SpotId isn't converted into a `RoutingId`, and the Node RID's hex representation isn't used as
  a domain identifier.
- A MeshNode's transport RID is an infrastructure identity distinct from a domain identifier. In a
  sample verifying routing id allocation, like Bingo and ZoneWorld, the location store automatically
  allocates the RID, and the application doesn't configure a fixed RID. The automatically allocated
  RID is confirmed via the public allocation result or a runtime observation result. The scope used
  as a Node direct target is limited to an Admin/Ops management operation. The NodeRid inside an
  `ActorRef` is only the current route snapshot — not an application identity or the target of the
  next call. This value doesn't substitute for a domain identifier like `RoomId`, `ConversationId`,
  `OrderId`, and isn't exposed to the client as a domain identifier.

## The Client Self-Check Standard

The Bingo and TicTacToe clients follow the common verification flow below. A sample client mustn't
stop at printing a success log — it must directly confirm that the value sent in a request comes
back with the same meaning in the response and push payload.

A per-language client must verify the following items.

- The token or actor id used in the authentication request matches the actor id in the
  authentication response.
- The RoomId and state status a room-creation or matching request returns match the request
  scenario. Also confirm that the endpoint list TicTacToe returns is a candidate for the client
  STREAM connection, not Room owner information. A field present only in a specific sample, like
  `GameName`, is confirmed only in that sample.
- The first participant receives a waiting status, and the second participant produces a running or
  in-progress status.
- Confirm a join notify that shouldn't be sent to yourself wasn't received.
- The other participant's join notify confirms the actor id, room id, and state status. A field
  present only in a specific sample, like TicTacToe's `Mark`, is additionally confirmed in that
  sample.
- Game start, move, draw, and ended notify confirm meaningful values in the payload, like board,
  turn, draw sequence, winner, player list — not just a simple receive count.
- A deterministic sample confirms the final winner and final state as fixed values.

Waiting for a push message directly calls the public wait API the stream connector object provides,
not a sample-local polling function. The wait flow must not be hidden behind a per-codec JSON,
MessagePack, Protobuf wrapper or a sample-specific function. The sample reads the payload with the
public interface of the message object the connector returned and verifies it right away. An inbox
or log queue for collecting notifications can exist for printing results and additional
verification, but must not become the standard path for waiting on push arrival.

## The Common Directory Structure For A State-Owning Server

Even though per-language syntax and build system differ, a state-owning server's source is aligned
to the structure below.

```text
Server/<StateOwner>/
  Domain/
    <DomainName>/
      ... pure domain rules ...
  Application/
    <UseCase>/
      ... use case services ...
  Infrastructure/
    ZLink/
      Actors/
      Handlers/
      Sessions/
      Spots/
        EntrySpot/
          Handlers/
        <DomainSpot>/
          Handlers/
          Notifications/
```

An unneeded directory can be omitted. If there's no Entry Spot, `EntrySpot/` doesn't need to exist,
and if a separate notification mapper isn't needed, `<DomainSpot>/Notifications/` doesn't need to
exist. Conversely, if bound-session push and domain-event conversion are needed, like in Bingo,
`Notifications/` is placed under that domain Spot.

The important standard isn't the name itself, but the dependency direction. `Domain` must not know
about `Application` or `Infrastructure`. `Application` uses the domain, but doesn't rely on
framework transport implementation details. `Infrastructure` handles framework objects, message
codec, logging, and DI registration, calling the domain object's methods rather than directly
manipulating domain state.
