# 04. Spot instance

[Reference index](README.en.md)

Completion kinds, capacity/timeout rules, and relocation ordering are exactly the same as
[Java reference 04. Spot instance](../../java/reference/04-spot-instance.ko.md) (Korean-only).
What Kotlin adds is a suspending lifecycle base class family (`ZLinkSuspendingSpot`) and
`ZLinkKotlinSpotManager`/`ZLinkKotlinRouteClient` extensions that wrap the same fluent state in
coroutines. The exact signatures are owned by the
[Kotlin Spot exact interface](../../common/spec/server/languages/kotlin/interfaces/spots.en.md)
(Korean-only).

---

## `ZLinkSuspendingSpot<TActor>` / `ZLinkSuspendingEntrySpot<TActor>` / `ZLinkSuspendingInstanceSpot`

Abstract base classes that override the Spot lifecycle as suspend functions. Since they inherit a
`final` bridge implementing Java's `ZLinkSpot`/`ZLinkEntrySpot`/`ZLinkInstanceSpot`, the
application only overrides the `*Suspending`-named methods.

```kotlin
class RoomSpot(override val context: ZLinkSpotContext) :
    ZLinkSuspendingSpot<PlayerActor>() {

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult = ZLinkSpotActorJoinResult.accept()

    override suspend fun onJoinedActorSuspending(actor: PlayerActor) { ... }
    override suspend fun onLeaveActorSuspending(actor: PlayerActor) { ... }
}
```

**Options.** The overridable suspend methods correspond one-to-one with the Spot lifecycle
callbacks in the Java reference's document 04 — `onCreateSuspending` (`ZLinkSuspendingSpot`
only), `onInitializeSuspending`, `onClosingSuspending`,
`onRelocationReadyCompletedSuspending`, `onActorJoinSuspending` (`ZLinkSuspendingSpot` only),
`onJoinedActorSuspending`, `onLeaveActorSuspending`, `onDisconnectActorSuspending`,
`onCreateActorSuspending` (`ZLinkSuspendingEntrySpot` only).

**Completion result.** `final override fun onCreate/onInitialize/...` only acts as a bridge
returning a Java `CompletionStage`, while the actual logic lives in the `*Suspending` suspend
methods. The completion kinds and ordering are the same as the Java reference's document 04.

**When to use.** Inherit this base class when implementing a Spot with Kotlin coroutines.
Implementing Java's `ZLinkSpot` directly requires handling `CompletionStage`/
`CompletableFuture` by hand.

---

## `ZLinkKotlinSpotManager.create` / `getOrCreate`

Creates a new User Spot, or reuses one if it exists. Wraps the same semantics as Java's
`ZLinkSpotManager` in a Kotlin-only single-use wrapper.

```kotlin
val created = spotManager.create("room")
    .inMesh("play")
    .request(CreateRoom("ranked"))
    .timeout(Duration.ofSeconds(5))
    .await()

val spotId = created.spot().spotId()
```

**Options.** In addition to `.inMesh(...)`, `.request(...)`, `.timeout(...)`, the terminal
`.await()`/`.yield()` — each with the same meaning as the `create`/`getOrCreate` entry in the
Java reference.

**Completion result.** Returns `ZLinkSpotCreateResult` (a Java type) as-is. Setting the same
option twice, or calling a terminal twice, is `InvalidOperation`.

**When to use.** Same as the `create`/`getOrCreate` entry in the Java reference. `find`/`close`
use the Java manager directly with no Kotlin-only wrapper (simple query/termination needs no
fluent state).

---

## `sendToSpot` / `requestToSpot` (ZLinkKotlinRouteClient extension)

Sends a one-way message, or exchanges a typed request/reply, to a single global SpotId. Provided
as extension functions of `ZLinkKotlinRouteClient`.

```kotlin
routeClient.sendToSpot("room-42", PlayerJoinedRoom("player-1")).await()

val reply = routeClient
    .requestToSpot<RoomState>("room-42", GetRoomState())
    .timeout(Duration.ofSeconds(3))
    .await()
```

**Options.** The modifiers `ZLinkKotlinSpotSendCall`/`ZLinkKotlinSpotRequestCall<TReply>`
provide are the same as `sendToSpot`/`requestToSpot` in the Java reference's document 04 —
`.metadata(...)`, `.instanceSpot()`/`.instanceSpot(stableType)`, `.inMesh(...)`, terminal
`.await()` (both) / `.yield()` (request only). The wrapper keeps this fluent state and ends the
Java call at the terminal.

**Completion result.** Same completion kinds as the Java reference (`NotFound`/
`TypeMismatch`/`DeadlineExceeded`, etc.).

**When to use.** Same as the `sendToSpot`/`requestToSpot` selection criteria in the Java
reference.

---

## `publish` (Spot Logical Multicast)

Publishes a typed event to subscribers by ChannelName and topic. Uses Java's
`ZLinkSpotPublisherClient`/`ZLinkSpotOutbound.publish(...)` directly and calls
`ZLinkPublishCall`'s `submit()` — there is no separate Kotlin-only coroutine wrapper. The
completion rules are the same as the
[`publish` entry in the Java reference's document 04](../../java/reference/04-spot-instance.ko.md)
(Korean-only).

---

## `addTimer` / `runCpuWorker` / `runIoWorker` (inside Spot code)

Uses Java's `ZLinkSpotContext.addTimer(...)`/`runCpuWorker(...)`/`runIoWorker(...)` directly, but
the timer handler is implemented as `ZLinkSuspendingSpotTimerHandler<TSpot>`
(`suspend fun handle(spot, tick)`), and the worker result is received via
`ZLinkKotlinWorkerCall<T>` (`suspend fun await()`/`yield()`).

**Completion result.** Same as the `addTimer`/`runCpuWorker`/`runIoWorker` entry in the Java
reference's document 04. The rule that logical timer registration is automatically carried over
on relocation is also identical.

**When to use.** Same as the Java reference — use `runCpuWorker` for CPU-bound work, and
`runIoWorker` for work that waits on I/O.

---

## Handler registration (`addHandler<T>()`, inside Spot code, `configure()`)

A reified extension function that registers a suspending handler type.

```kotlin
override fun configure() {
    context.handlers().addHandler<StartGameHandler>()
}
```

**Options.** `ZLinkSpotHandlerRegistry.addHandler<THandler>()` internally delegates to Java's raw
`Class<?>`-based registration. The interface the handler implements
(`ZLinkSuspendingSpotPacketHandler`, `ZLinkSuspendingSpotRequestHandler`,
`ZLinkSuspendingSpotSubscriptionHandler`, `ZLinkSuspendingSpotActorSendHandler`,
`ZLinkSuspendingSpotActorRequestHandler`) determines the actual role — the correspondence is the
same as the handler registration table in the Java reference's document 04.

**Completion result.** Same as the Java reference — registers synchronously with no return
value, and a duplicate handler key under the same owner surfaces in startup validation.

**When to use.** Registers every suspending handler this Spot will process, each time
`configure()` is called.

---

## `leaveActor` / `close` / `destroyActor` / `relocationReady().defer()` (inside Spot code)

Calls the same-named methods of Java's `ZLinkSpotContext`/`ZLinkEntrySpotContext`/
`ZLinkInstanceSpotContext` directly (returning Java's `CompletionStage`, not `suspend` — chain
with `.await()` if needed). `relocationReady().defer()` is also identical to Java, and
`onRelocationReadyCompletedSuspending(...)` receives the completion. The completion rules are the
same as the corresponding entry in
[the Java reference's document 04](../../java/reference/04-spot-instance.ko.md) (Korean-only).

---

See the
[Kotlin Spot exact interface](../../common/spec/server/languages/kotlin/interfaces/spots.en.md)
and
[Java reference 04. Spot instance](../../java/reference/04-spot-instance.ko.md) (Korean-only)
for the full rationale.
