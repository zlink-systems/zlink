# 05. Actor relocation

[Reference index](README.en.md)

Completion kinds, capacity/timeout rules, and relocation ordering are exactly the same as
[Java reference 05. Actor relocation](../../java/reference/05-actor-relocation.ko.md)
(Korean-only). What Kotlin adds is a suspending Actor base class, and
`ZLinkKotlinActorManager`/`ZLinkKotlinActorClient`, which wrap the same fluent state in
coroutines. The exact signatures are owned by the
[Kotlin Actor exact interface](../../common/spec/server/languages/kotlin/interfaces/actors.en.md)
(Korean-only).

---

## `ZLinkSuspendingActor` / `ZLinkSuspendingActorFactory`

An abstract base class and factory that override the Actor lifecycle as suspend functions.

```kotlin
class PlayerActor(override val context: ZLinkActorContext) : ZLinkSuspendingActor() {
    override suspend fun onJoinCompletedSuspending(
        completion: ZLinkActorJoinCompletion,
    ) { ... }
}

class PlayerActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        PlayerActor(context)
}
```

**Options.** `ZLinkSuspendingActor` overrides `context: ZLinkActorContext` (a property) and
`onJoinCompletedSuspending(completion)`. `ZLinkSuspendingActorFactory` overrides
`createActor(context)`.

**Completion result.** `final override fun context()`/`onJoinCompleted(...)`/`create(...)` only
act as a Java `CompletionStage` bridge, while the actual logic lives in the suspend methods.

**When to use.** Inherit this base class when implementing an Actor and factory with Kotlin
coroutines.

---

## `ZLinkKotlinActorManager.create` / `getOrCreate`

Creates a new Actor, or reuses one if it exists.

```kotlin
val created = actorManager.create("player-1", "player")
    .inMesh("play")
    .request(SpawnPlayer("player-1"))
    .await()
```

**Options.** In addition to `.inMesh(...)`, `.request(...)`, `.timeout(...)`, the terminal
`.await()`/`.yield()` — with the same meaning as the `create`/`getOrCreate` entry in the Java
reference.

**Completion result.** Returns `ZLinkActorCreateResult` (a Java type) as-is. `yield()` returns
the current Spot gate only from a `SPOT_WIDE` User Spot/Instance Spot application callback — in
any other context it ends with `InvalidOperation` before reservation, factory execution, and
queue changes.

**When to use.** Same as the `create`/`getOrCreate` entry in the Java reference. `find`/
`findSpot`/`destroy` use the Java manager directly with no Kotlin-only wrapper.

---

## `sendToActor` / `requestToActor` (ZLinkKotlinActorClient)

Sends a one-way message, or exchanges a typed request/reply, to a single global ActorId.

```kotlin
actorClient.sendToActor("player-1", GrantItem("sword")).await()

val reply = actorClient
    .requestToActor<Inventory>("player-1", GetInventory())
    .timeout(Duration.ofSeconds(3))
    .await()
```

**Options.** `sendToActor` returns `ZLinkKotlinMessageSendCall` (`.metadata(...)`, `.await()`),
and `requestToActor` returns `ZLinkKotlinRequestCall<TReply>` (`.metadata(...)`,
`.timeout(...)`, `.await()`/`.yield()`). `requestToActor<TReply>(actorId, request)` is a reified
inline extension. Actor send only provides one-way `await(): Unit` and does not provide
`yield()`.

**Completion result.** Same completion kinds as the Java reference's `sendToActor`/
`requestToActor`.

**When to use.** Use `sendToActor` if no reply is needed, and `requestToActor` if one is.

---

## `joinSpot` / `joinEntrySpot` (inside Actor code)

Joins the current Actor to a User Spot or an Entry Spot. Uses Java's
`ZLinkActorContext.joinSpot(...)`/`joinEntrySpot(...)` directly, and the only terminal is the
same synchronous `defer()` as Java — no coroutine terminal is added.

```kotlin
context.joinSpot("room-42", JoinRoomRequest("player-1"))
    .timeout(Duration.ofSeconds(5))
    .defer()
```

**Completion result.** The result is delivered asynchronously via
`onJoinCompletedSuspending(...)` (`ZLinkActorJoinCompletion`: `Accepted`/`Rejected`/`Failed`).
`defer()` is called once during handler execution and does not release the Spot gate or Actor
FIFO claim.

**When to use.** Same as the `joinSpot`/`joinEntrySpot` entry in the Java reference. Calling it
from an Actor in an Entry Spot or a `PER_ACTOR` User Spot completes with `InvalidOperation`.

---

## Relocation policy selection (at Actor factory registration time)

Registers Java's `ZLinkActorRelocationAdapter<TActor>` via
`preserveStateWith(AdapterClass::class.java)`. The opaque Java `byte[]` appears as Kotlin
`ByteArray`, and `capture`/`restore` return the same `CompletionStage` as Java — there is no
Kotlin-only suspending adapter.

| Policy | When to use |
| --- | --- |
| `disableRelocation()` | When this Actor must never be moved to another node |
| `recreateOnRelocation()` | When an Actor may be recreated without state |
| `preserveStateWith(AdapterClass::class.java)` | When state must be preserved across the move |

**Completion result.** Follows the same completion rules as the relocation-policy-selection entry
in the Java reference (capture result capped at 64 MiB, retry-safe requirement, etc.). If the
adapter's target type differs from the factory's Actor type, it is a configuration error before
the socket bind.

**When to use.** Same as the Java reference — decided once, at factory registration time. Kotlin
adds no reified helper or overload that omits the policy.

---

See the
[Kotlin Actor exact interface](../../common/spec/server/languages/kotlin/interfaces/actors.en.md)
and
[Java reference 05. Actor relocation](../../java/reference/05-actor-relocation.ko.md)
(Korean-only) for the full rationale.
