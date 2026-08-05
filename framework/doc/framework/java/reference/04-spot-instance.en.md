# 04. Spot instance

[Reference index](README.en.md)

This category covers the external entry points `ZLinkSpotManager`/`ZLinkRouteClient`/
`ZLinkSpotPublisherClient` provide, and the entry points used inside Spot code via
`ZLinkSpotContext`/`ZLinkInstanceSpotContext`. The exact signatures are owned by the
[Java Spot exact interface](../../common/spec/server/languages/java/interfaces/spots.en.md)
(Korean-only).

---

## `ZLinkSpotManager.create`

Always creates a new User Spot. The Framework issues a new global SpotId.

```java
ZLinkSpotCreateResult created = spotManager.create("room")
    .inMesh("play")
    .request(new CreateRoom("ranked"))
    .timeout(Duration.ofSeconds(5))
    .submit()
    .toCompletableFuture().get();

String spotId = created.spot().spotId();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.inMesh(meshName)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to create the Spot in. Omitting it with two or more candidates completes with `INVALID_OPERATION`; none completes with `NOT_CONFIGURED`; a nonexistent specified Mesh completes with `NOT_FOUND` |
| `.request(Object)` / `.request(ZLinkMessage)` | None (empty request) | The creation request passed to the Spot's `onCreate(...)` |
| `.timeout(Duration)` | Default applied to the whole of resolve/factory/initialize | The upper bound until the entire creation reaches a terminal state |
| `.submit()` | terminal (pick one) | Waits until creation completes |
| `.yield()` | terminal (pick one) | Only valid inside a `SpotWide` handler |

**Completion result.** `ZLinkSpotCreateResult.state()` is `CREATED` (newly created). If the
Spot's `onCreate(...)` rejects it, it is `REJECTED` and `reply()` carries the rejection message.
Setting the same option twice, or calling a terminal twice, completes with `INVALID_OPERATION`;
not finishing within the deadline completes with `DEADLINE_EXCEEDED`.

**When to use.** Use this when a new instance is always needed. Use `getOrCreate` to reuse an
existing one and only create when there is none.

---

## `ZLinkSpotManager.getOrCreate`

Returns the Ready Spot with the specified SpotId if it exists, and creates a new one otherwise.

```java
ZLinkSpotCreateResult existingOrCreated = spotManager.getOrCreate("lobby-eu", "lobby")
    .inMesh("play")
    .request(new CreateLobby("eu"))
    .submit()
    .toCompletableFuture().get();
```

**Options.** The same as `create` — `.inMesh(...)`, `.request(...)`, `.timeout(...)`, terminal
`.submit()` or `.yield()`.

**Completion result.** If `state()` is `EXISTING`, it returns the already-existing Spot as-is and
ignores `request`. `CREATED` means a new one was made. If the same SpotId is currently contended
in a creating state, it waits for that result and joins it; if cleanup makes it missing, it
re-competes for a new reservation.

**When to use.** Use this when an idempotent "use if it exists, create if it doesn't" by SpotId
is needed. Use `create` if a new instance is always needed.

---

## `find` / `close` (manager)

Queries an existing Spot, or closes the exact incarnation.

```java
Optional<SpotRef> spot = spotManager.find("lobby-eu").toCompletableFuture().get();

if (spot.isPresent()) {
    boolean closed = spotManager.close(spot.get()).toCompletableFuture().get();
}
```

**Options.** Neither call has modifiers — both only take the target identifier.

**Completion result.** `find` returns `Optional.empty()` if there is no Ready Spot. `close`
returns `false` if the incarnation does not exist, completes with `INVALID_OPERATION` if the
generation differs, and `UNAVAILABLE` while a pre-commit seal is in progress. If a User Spot
still has Actor membership, it returns `false` and does not automatically leave/destroy the
Actor.

**When to use.** Use this when you need to check current existence or explicitly terminate a
Spot. `close` does not close a different incarnation on behalf of a stale `SpotRef`.

---

## `sendToSpot`

Sends a one-way message to a single global SpotId. The external client (`ZLinkRouteClient`) and
Spot code (`ZLinkSpotOutbound`) provide the same shape.

```java
routeClient.sendToSpot("room-42", new PlayerJoinedRoom("player-1")).submit();

// activating a new Instance Spot on demand (cold activation) before sending
routeClient.sendToSpot("device-42", new DeviceCommand("reboot"))
    .instanceSpot("device")
    .submit();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.metadata(key, value)` | None | Key-value to pass to the handler |
| `.instanceSpot()` | None (resolves User Spot only) | Performs cold activation if missing. If an existing authority already exists, it uses the stored type regardless of the number of stable types |
| `.instanceSpot(stableType)` | — | If missing and several types are registered, the stable type must be specified |
| `.inMesh(meshName)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to first create a missing Instance Spot in. Using it without an instance marker completes with `INVALID_OPERATION` |
| `.submit()` | Required terminal | Waits only until source-local admission |

**Completion result.** No SpotId and no Instance marker completes with `NOT_FOUND`. If
`.instanceSpot(...)` was used but the existing authority is a User Spot, or differs from the
specified type, it completes with `TYPE_MISMATCH`. Other completion kinds follow the same common
rules as the messaging-execution category.

**When to use.** Use this for Spot messaging where no reply is needed. Use `requestToSpot` if a
reply is needed.

---

## `requestToSpot`

Sends and receives a typed request/reply to a single global SpotId.

```java
CompletionStage<RoomState> reply = routeClient
    .requestToSpot("room-42", new GetRoomState())
    .timeout(Duration.ofSeconds(3))
    .submit(RoomState.class);
```

**Options.** In addition to the same `.instanceSpot(...)`/`.inMesh(...)` as `sendToSpot`, this
adds the following.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(Duration)` | The MeshNode's request default timeout | The deadline covering resolve, cold activation, handler, and reply altogether |
| `.submit(TReply.class)` | terminal (pick one) | Waits until the reply arrives |
| `.yield(TReply.class)` | terminal (pick one) | Only valid inside a `SpotWide` User Spot/Instance Spot handler. Calling it elsewhere completes with `INVALID_OPERATION` |

**Completion result.** In addition to the same failure kinds as `sendToSpot`, if the factory or
initialize fails during cold activation, it completes as a typed failure — the Framework does not
retry internally.

**When to use.** Use this when the reply value is needed. Use `sendToSpot` if it is one-way.

---

## `publish` (Spot Logical Multicast)

Publishes a typed event to subscribers by ChannelName and topic. `ZLinkSpotPublisherClient`
(external) and `ZLinkSpotOutbound` (inside Spot code) provide the same shape.

```java
spotPublisherClient.publish("room.events", "room-42", new RoomStateChanged("started"))
    .submit();
```

**Options.** This call has `.metadata(...)` and the required terminal `.submit()` — the topic is
a required argument.

**Completion result.** A normal completion means publish admission finished. It does not wait for
subscriber reception. Unlike classic fanout `publish` in the messaging-execution category, the
owner MeshNode is determined by ChannelName alone, and the caller does not pass a MeshName
separately.

**When to use.** Use this to notify observers of a Spot state change. If a direct reply from a
subscriber is needed, use `requestToSpot` instead of this entry.

---

## `addTimer` (inside Spot code)

Registers a periodic timer belonging to a Spot. Called via `ZLinkSpotContext.addTimer(...)`.

```java
ZLinkTimer timer = context.addTimer(
    "room-tick",
    Duration.ofSeconds(1),
    RoomTickHandler.class,
    new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
    .toCompletableFuture().get();
```

**Options.** The components of `ZLinkTimerOptions` are as follows.

| Component | Default | Meaning |
| --- | --- | --- |
| `overrunPolicy` | `SKIP_LATE_TICKS` | Whether to skip when a tick falls behind, catch up within a bound, or delay the next tick |
| `maxCatchUpTicks` | 1 | The maximum ticks to catch up at once when `CATCH_UP_BOUNDED` |
| `stopOnUnhandledException` | `false` | Whether to stop the timer on a handler exception |

**Completion result.** Returns a `ZLinkTimer`. Because the timer is a logical registration
belonging to this Spot, it is automatically carried over on relocation and the application does
not need to re-register it at the target. Cancel it with `cancel()` or `close()`
(`AutoCloseable`).

**When to use.** Use this when a Spot needs periodic work.

---

## `runCpuWorker` / `runIoWorker` (inside Spot code)

Runs work on a separate worker without blocking the Spot's owner turn.

```java
CompletionStage<Integer> result = context
    .runCpuWorker(cancellation -> computeExpensiveScore(cancellation))
    .timeout(Duration.ofSeconds(2))
    .submit();
```

**Options.** `ZLinkWorkerCall<T>` provides the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(Duration)` | The worker option's default | The upper bound for the work to complete |
| `.submit()` | terminal (pick one) | Waits until completion |
| `.yield()` | terminal (pick one) | Only valid inside a `SpotWide` handler |

**Completion result.** Returns `T`, or completes with `DEADLINE_EXCEEDED` on timeout. The worker
pool size and idle timeout are configured only before the host starts (`configureWorkers()`).

**When to use.** Use `runCpuWorker` for CPU-bound computation, and `runIoWorker` (taking a
`ZLinkIoWorkerTask` — returning `CompletionStage<T>`) for work that waits on I/O. Both exist to
avoid blocking the owner turn's sequential execution.

---

## Handler registration (inside Spot code, `configure()`)

Registers the handlers that process the packets/requests/subscriptions/member Actor messages a
Spot receives. Called via `ZLinkSpotContext.handlers()` (User Spot)/
`ZLinkInstanceSpotContext.handlers()` (Instance Spot), and only from inside the `configure()`
override.

```java
@Override
public void configure() {
    context().handlers().addHandler(StartGameHandler.class);
}
```

**Options.** The interface implemented and the annotation used differ by what the handler
processes.

| Target | Handler interface | Identifying annotation |
| --- | --- | --- |
| One-way packet in front of a User Spot | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `@ZLinkPacket` |
| Request in front of a User Spot | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `@ZLinkSpotRequest` |
| A Logical Multicast subscription event | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `@ZLinkSpotSubscription(spotNodeName, topic)` |
| A one-way packet in front of a User Spot's member Actor | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `@ZLinkSpotActorSend` |
| A request in front of a User Spot's member Actor | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `@ZLinkSpotActorRequest` |
| A Spot's periodic timer | `ZLinkSpotTimerHandler<TSpot>` | `@ZLinkSpotTimer(name, periodMillis)` |
| A packet in front of an Instance Spot | The same shape as a User Spot's packet handler | `@ZLinkPacket` |

`ZLinkSpotHandlerRegistry.addHandler(Class<?>)` and
`ZLinkInstanceSpotHandlerRegistry.addPacket(Class<?>)` are single registration methods that do not
distinguish handler kind — the annotation and implemented interface determine the actual role.

**Completion result.** Registers synchronously with no return value. Omitting the packet name
uses the annotation's `value()`/`packetName()`, and the type name if there is no annotation
either. A duplicate handler key under the same owner surfaces as a `ZLinkConfigurationException`
in startup validation.

**When to use.** Registers every handler this Spot will process, each time `configure()` is
called. See the registration entries in the topology-discovery category for Node/Channel
handlers, and the stream-session category for STREAM session handlers.

---

## `outbound()` — `sendToChannel` / `requestToChannel` (inside Spot code)

Sends a one-way message by ChannelName, or exchanges a typed request/reply, from inside Spot
code. Provided by the `ZLinkSpotOutbound` that `ZLinkSpotContext.outbound()` returns, in the same
shape as `sendToChannel`/`requestToChannel` in the messaging-execution category.

```java
CompletionStage<Leaderboard> reply = context.outbound()
    .requestToChannel("leaderboard.api", new GetLeaderboard())
    .submit(Leaderboard.class);
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

```java
context.leaveActor(actor).toCompletableFuture().get();        // User Spot: only removes the member Actor
boolean closed = context.close().toCompletableFuture().get(); // User/Instance Spot: closes this Spot itself
entryContext.destroyActor(actor).toCompletableFuture().get();  // Entry Spot: destroys the Actor entirely
```

**Options.** None of the three calls has modifiers — they only take the target
(`leaveActor`/`destroyActor`).

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

In a `SPOT_WIDE` Spot that has chosen `APPLICATION_SIGNALED` readiness mode, defers the
relocation boundary to just before the next application turn.

```java
context.relocationReady().defer();
```

**Options.** This call has no modifiers.

**Completion result.** No return value. Registers the relocation boundary after the current
handler ends. If it did not move, or aborted before commit, it receives a `CONTINUED` completion
at the source; if it moved, it receives a `RELOCATED` completion at the target, via
`onRelocationReadyCompleted(...)`. `ANY_TURN_BOUNDARY` mode, a `PER_ACTOR` Spot, an
Entry/Instance Spot, outside a Spot turn, or a duplicate call in the same turn all complete with
`INVALID_OPERATION`.

**When to use.** Use this when the application must precisely control the relocation moment down
to a specific turn boundary. This call is not needed under the default `ANY_TURN_BOUNDARY` mode.

---

See the
[Java Spot exact interface](../../common/spec/server/languages/java/interfaces/spots.en.md)
(Korean-only) for the full rationale.
