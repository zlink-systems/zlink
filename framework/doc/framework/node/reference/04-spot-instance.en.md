# 04. Spot instance

[Reference index](README.en.md)

This category covers the external entry points `ZLinkSpotManager` (`ZLINK_SPOT_MANAGER`)/
`ZLinkRouteClient`/`ZLinkSpotPublisherClient` (`ZLINK_SPOT_PUBLISHER_CLIENT`) provide, and the
entry points used inside Spot code via `ZLinkSpotContext`/`ZLinkInstanceSpotContext`. The exact
signatures are owned by the
[Spot and Instance Spot exact interface](../../common/spec/server/languages/node/interfaces/04-spots.en.md)
and the
[STREAM, timer, and worker exact interface](../../common/spec/server/languages/node/interfaces/06-stream-worker.en.md)
(Korean-only).

---

## `ZLinkSpotManager.create`

Always creates a new User Spot. The Framework issues a new global SpotId.

```ts
const created = await spotManager
  .create("room")
  .inMesh("play")
  .request(new CreateRoom("ranked"))
  .timeout(5_000)
  .submit();

const spotId = created.spot.spotId;
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.inMesh(meshName)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to create the Spot in. Omitting it with two or more candidates completes with `InvalidOperation`; none completes with `NotConfigured`; a nonexistent specified Mesh completes with `NotFound` |
| `.request(request)` | None (empty request) | The creation request passed to the Spot's `onCreate(...)` |
| `.timeout(timeoutMs)` | Default applied to the whole of resolve/factory/initialize | The upper bound until the entire creation reaches a terminal state |
| `.submit(signal?)` | terminal (pick one) | Waits until creation completes |
| `.yield(signal?)` | terminal (pick one) | Only valid inside a `SpotWide` handler |

**Completion result.** `ZLinkSpotCreateResult.state` is `"created"` (newly created). If the
Spot's `onCreate(...)` rejects it, it is `"rejected"` and `reply` carries the rejection message.
Setting the same option twice, or calling a terminal twice, completes with `InvalidOperation`;
not finishing within the deadline completes with `DeadlineExceeded`.

**When to use.** Use this when a new instance is always needed. Use `getOrCreate` to reuse an
existing one and only create when there is none.

---

## `ZLinkSpotManager.getOrCreate`

Returns the Ready Spot with the specified SpotId if it exists, and creates a new one otherwise.

```ts
const existingOrCreated = await spotManager
  .getOrCreate("lobby-eu", "lobby")
  .inMesh("play")
  .request(new CreateLobby("eu"))
  .submit();
```

**Options.** The same as `create` — `.inMesh(...)`, `.request(...)`, `.timeout(...)`, terminal
`.submit(signal?)` or `.yield(signal?)`.

**Completion result.** If `state` is `"existing"`, it returns the already-existing Spot as-is and
ignores `request`. `"created"` means a new one was made. If the same SpotId is currently
contended in a creating state, it waits for that result and joins it; if cleanup makes it
missing, it re-competes for a new reservation.

**When to use.** Use this when an idempotent "use if it exists, create if it doesn't" by SpotId
is needed. Use `create` if a new instance is always needed.

---

## `find` / `close` (manager)

Queries an existing Spot, or closes the exact incarnation.

```ts
const spot = await spotManager.find("lobby-eu");

if (spot) {
  const closed = await spotManager.close(spot);
}
```

**Options.** Neither call has modifiers — both only take the target identifier.

**Completion result.** `find` returns `undefined` if there is no Ready Spot. `close` returns
`false` if the incarnation does not exist, completes with `InvalidOperation` if the generation
differs, and `Unavailable` while a pre-commit seal is in progress. If a User Spot still has Actor
membership, it returns `false` and does not automatically leave/destroy the Actor.

**When to use.** Use this when you need to check current existence or explicitly terminate a
Spot. `close` does not close a different incarnation on behalf of a stale `SpotRef`.

---

## `sendToSpot`

Sends a one-way message to a single global SpotId. The external client (`ZLinkRouteClient`) and
Spot code (`ZLinkSpotOutbound`) provide the same shape.

```ts
await routeClient
  .sendToSpot("room-42", new PlayerJoinedRoom("player-1"))
  .submit();

// activating a new Instance Spot on demand (cold activation) before sending
await routeClient
  .sendToSpot("device-42", new DeviceCommand("reboot"))
  .instanceSpot("device")
  .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the handler |
| `.instanceSpot()` | None (resolves User Spot only) | Performs cold activation if missing. If an existing authority already exists, it uses the stored stable type regardless of the number of registered types |
| `.instanceSpot(instanceSpotType)` | — | If missing and several types are registered, the stable type must be specified |
| `.inMesh(meshName)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to first create a missing Instance Spot in. Using it without an instance marker completes with `InvalidOperation` |
| `.submit(signal?)` | Required terminal | Waits only until source-local admission |

**Completion result.** No SpotId and no Instance marker completes with `NotFound`. If
`.instanceSpot(...)` was used but the existing authority is a User Spot, or differs from the
specified type, it completes with `TypeMismatch`. Other completion kinds follow the same common
rules as the messaging-execution category.

**When to use.** Use this for Spot messaging where no reply is needed. Use `requestToSpot` if a
reply is needed.

---

## `requestToSpot`

Sends and receives a typed request/reply to a single global SpotId.

```ts
const reply = await routeClient
  .requestToSpot("room-42", new GetRoomState())
  .timeout(3_000)
  .submit<RoomState>();
```

**Options.** In addition to the same `.instanceSpot(...)`/`.inMesh(...)` as `sendToSpot`, this
adds the following.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(timeoutMs)` | The MeshNode's request default timeout | The deadline covering resolve, cold activation, handler, and reply altogether |
| `.submit<TReply>(signal?)` | terminal (pick one) | Waits until the reply arrives |
| `.yield<TReply>(signal?)` | terminal (pick one) | Only valid inside a `SpotWide` User Spot/Instance Spot handler. Calling it elsewhere completes with `invalidConfiguration` |

**Completion result.** In addition to the same failure kinds as `sendToSpot`, if the factory or
initialize fails during cold activation, it completes as a typed failure — the Framework does not
retry internally.

**When to use.** Use this when the reply value is needed. Use `sendToSpot` if it is one-way.

---

## `publish` (Spot Logical Multicast)

Publishes a typed event to subscribers by ChannelName and topic. `ZLinkSpotPublisherClient`
(external) and `ZLinkSpotOutbound` (inside Spot code) provide the same shape.

```ts
await spotPublisherClient
  .publish("room.events", "room-42", new RoomStateChanged("started"))
  .submit();
```

**Options.** This call has `.metadata(...)` and the required terminal `.submit(signal?)` — the
topic is a required argument.

**Completion result.** A normal completion means publish admission finished. It does not wait for
subscriber reception. Unlike classic fanout `publish` in the messaging-execution category, the
owner MeshNode is determined by ChannelName alone, and the caller does not pass a MeshName
separately.

**When to use.** Use this to notify observers of a Spot state change. If a direct reply from a
subscriber is needed, use `requestToSpot` instead of this entry.

---

## `addTimer` (inside Spot code)

Registers a periodic timer belonging to a Spot. Called via
`ZLinkSpotCommonContext.addTimer(...)`.

```ts
const timer = await context.addTimer(
  "room-tick",
  1_000,
  RoomTickHandler,
  { overrunPolicy: ZLinkTimerOverrunPolicy.SkipLateTicks },
);
```

**Options.** The fields of `ZLinkTimerOptions` are as follows.

| Field | Default | Meaning |
| --- | --- | --- |
| `overrunPolicy` | `SkipLateTicks` | Whether to skip when a tick falls behind, catch up within a bound, or delay the next tick |
| `maxCatchUpTicks` | 1 | The maximum ticks to catch up at once when `CatchUpBounded` |
| `stopOnUnhandledException` | `false` | Whether to stop the timer on a handler exception |

**Completion result.** Returns a `ZLinkTimer`. Because the timer is a logical registration
belonging to this Spot, it is automatically carried over on relocation and the application does
not need to re-register it at the target. Cancel it with `cancel(signal?)` or `dispose()`.

**When to use.** Use this when a Spot needs periodic work.

---

## `runCpuWorker` / `runIoWorker` (inside Spot code)

Runs work on a separate worker without blocking the Spot's owner turn.

```ts
const result = await context
  .runCpuWorker((signal) => computeExpensiveScore(signal))
  .timeoutMs(2_000)
  .submit();
```

**Options.** `ZLinkWorkerCall<T>` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeoutMs(durationMs)` | The worker option's default | The upper bound for the work to complete |
| `.submit(signal?)` | terminal (pick one) | Waits until completion |
| `.yield(signal?)` | terminal (pick one) | Only valid inside a `SpotWide` handler |

**Completion result.** Returns `T`, or completes with `DeadlineExceeded` on timeout. The worker
pool size (`minThreads`/`maxThreads`) and idle timeout are configured only before the host starts.

**When to use.** Use `runCpuWorker` for CPU-bound computation, and `runIoWorker` (returning
`Promise<T>`) for work that waits on I/O. Both exist to avoid blocking the owner turn's sequential
execution.

---

## Handler registration (inside Spot code, decorator)

Marks a handler class that will process the packets/requests/subscriptions/member Actor messages
a Spot receives, using a decorator. NestJS provider discovery (`zlinkDiscoverProviders(...)`)
finds handler classes inside the module and registers them.

```ts
@zlinkSpotRequestHandler({ spot: () => RoomSpot, packetName: "start-game" })
export class StartGameHandler implements ZLinkSpotRequestHandler<RoomSpot, StartGameReq, StartGameRes> {
  handle(spot: RoomSpot, request: StartGameReq): Promise<StartGameRes> { ... }
}
```

**Options.** The interface implemented and the decorator used differ by what the handler
processes.

| Target | Handler interface | Decorator |
| --- | --- | --- |
| One-way packet in front of a User Spot | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `@zlinkSpotPacketHandler({ spot, packetName? })` |
| Request in front of a User Spot | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `@zlinkSpotRequestHandler({ spot, packetName })` (a `@ZLinkSpotRequest(packetName?)` method decorator in the raw builder) |
| A Logical Multicast subscription event | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `@zlinkSpotSubscriptionHandler({ spot, channelName, topic })` (raw is `@ZLinkSpotSubscription(channelName, topic)`) |
| A Spot's periodic timer | `ZLinkSpotTimerHandler<TSpot>` | `@zlinkSpotTimerHandler({ spot?, name?, periodMs?, options? })` |
| A one-way packet in front of a User Spot's member Actor | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `@zlinkSpotActorSendHandler({ spot, actor, packetName })` (raw is `@ZLinkSpotActorSend(packetName?)`) |
| A request in front of a User Spot's member Actor | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `@zlinkSpotActorRequestHandler({ spot, actor, packetName })` |
| A one-way packet/request in front of an Entry Spot's member Actor | `ZLinkEntrySpotActorSendHandler`/`ZLinkEntrySpotActorRequestHandler` | `@zlinkEntrySpotActorSendHandler`/`@zlinkEntrySpotActorRequestHandler({ entrySpot, actor, packetName })` |
| An Entry Spot's own packet/subscription | The same shape as `ZLinkSpotPacketHandler`/`ZLinkSpotSubscriptionHandler` | `@zlinkEntrySpotPacketHandler`/`@zlinkEntrySpotSubscriptionHandler({ entrySpot, ... })` |
| A packet in front of an Instance Spot (raw builder) | `ZLinkInstanceSpotHandlerRegistry.addPacket(handlerType)` | None (direct registration) |

**Completion result.** Registers synchronously with no return value. The `spot`/`entrySpot`
field is `Type<T> | (() => Type<T>)` — use a lazy resolver (`() => RoomSpot`) to avoid a circular
reference. A duplicate handler key under the same owner surfaces as a configuration error in
startup validation.

**When to use.** Attach exactly one decorator to every Spot handler class. See the registration
entries in the topology-discovery category for Node/Channel handlers, and the stream-session
category for STREAM session handlers.

---

## `outbound` — `sendToChannel` / `requestToChannel` (inside Spot code)

Sends a one-way message by ChannelName, or exchanges a typed request/reply, from inside Spot
code. Provided by `ZLinkSpotCommonContext.outbound`, in the same shape as `sendToChannel`/
`requestToChannel` in the messaging-execution category.

```ts
const reply = await context.outbound
  .requestToChannel("leaderboard.api", new GetLeaderboard())
  .submit<Leaderboard>();
```

**Options.** Takes the same modifiers as `sendToChannel`/`requestToChannel` in the
messaging-execution category.

**Completion result.** Same as the completion kinds in the messaging-execution category.

**When to use.** Use this when a Spot must call a handler on a different ChannelName from inside
its own code, rather than an external client doing so. Use `sendToSpot`/`requestToSpot` to call
another Spot directly.

---

## `leaveActor` / `close` / `destroyActor` (inside Spot code, termination/departure)

Removes a member Actor from this Spot, closes the Spot itself, or destroys an Actor from an Entry
Spot.

```ts
await context.leaveActor(actor);        // User Spot: only removes the member Actor
const closed = await context.close();   // User/Instance Spot: closes this Spot itself
await entryContext.destroyActor(actor); // Entry Spot: destroys the Actor entirely
```

**Options.** None of the three calls has modifiers — they only take the target
(`leaveActor`/`destroyActor`) and an optional `signal`.

**Completion result.** `leaveActor` (`ZLinkSpotContext` only) only releases member Actor
membership and does not destroy the Actor itself. `close` (`ZLinkSpotContext`/
`ZLinkInstanceSpotContext`) uses the same completion kinds as the manager's `close(spotRef)` (the
earlier entry in the spot-instance category), but targets this Spot itself. `destroyActor`
(`ZLinkEntrySpotContext` only) destroys the Actor entirely — unlike `leaveActor`, it removes the
Actor itself rather than releasing membership.

**When to use.** Use `leaveActor` to remove a member Actor from this Spot without moving it
elsewhere, `close` to terminate the Spot itself, and `destroyActor` to entirely remove an Actor
that is no longer needed at an Entry Spot.

---

## `relocationReady().defer()` (inside Spot code)

In a `SpotWide` Spot that has chosen `ApplicationSignaled` readiness mode, defers the relocation
boundary to just before the next application turn.

```ts
context.relocationReady().defer();
```

**Options.** This call has no modifiers.

**Completion result.** No return value. Registers the relocation boundary after the current
handler ends. If it did not move, or aborted before commit, it receives a `Continued` completion
at the source; if it moved, it receives a `Relocated` completion at the target, via the optional
`onRelocationReadyCompleted(...)` (completing as a no-op if there is no callback).
`AnyTurnBoundary` mode, a `PerActor` Spot, an Entry/Instance Spot, outside a Spot turn, or a
duplicate call in the same turn all complete with `InvalidOperation`.

**When to use.** Use this when the application must precisely control the relocation moment down
to a specific turn boundary. This call is not needed under the default `AnyTurnBoundary` mode.

---

See the
[Spot and Instance Spot exact interface](../../common/spec/server/languages/node/interfaces/04-spots.en.md)
and the
[STREAM, timer, and worker exact interface](../../common/spec/server/languages/node/interfaces/06-stream-worker.en.md)
(Korean-only) for the full rationale.
