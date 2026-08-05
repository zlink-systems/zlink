# 05. Actor relocation

[Reference index](README.en.md)

This category covers the external entry points `ZLinkActorManager`/`ZLinkActorClient` provide,
the entry point for joining a Spot from inside Actor code via `ZLinkActorContext`, and relocation
policy selection. The exact signatures are owned by the
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.en.md)
(Korean-only).

---

## `ZLinkActorManager.create`

Always creates a new Actor.

```java
ZLinkActorCreateResult created = actorManager.create("player-1", "player")
    .inMesh("play")
    .request(new SpawnPlayer("player-1"))
    .submit()
    .toCompletableFuture().get();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.inMesh(meshName)` | Optional if exactly one Mesh has Object Client/Server role | The Mesh to create the Actor in. Omitting it with two or more candidates completes with `INVALID_OPERATION`; none completes with `NOT_CONFIGURED`; a nonexistent specified Mesh completes with `NOT_FOUND` |
| `.request(Object)` / `.request(ZLinkMessage)` | None (empty request) | The request passed at Actor factory creation time |
| `.timeout(Duration)` | 5 seconds | The deadline covering resolve/reservation/factory/Ready barrier altogether |
| `.submit()` | terminal (pick one) | Waits until creation completes |
| `.yield()` | terminal (pick one) | Only valid inside a `SpotWide` handler |

**Completion result.** `ZLinkActorCreateResult` (a sealed interface) completes as one of
`Created` (newly created) or `Rejected` (the factory rejected it). If a Ready incarnation of the
same ActorId already exists, it completes with an `ALREADY_EXISTS` error rather than either
alternative — `Existing` only exists for `getOrCreate`. If a Ready incarnation exists but its
stable type differs, it is `TYPE_MISMATCH`.

**When to use.** Use this when a new Actor is always needed. Use `getOrCreate` to reuse an
existing one and only create when there is none.

---

## `ZLinkActorManager.getOrCreate`

Returns the Ready Actor with the same ActorId if it exists, and creates a new one otherwise.

```java
ZLinkActorCreateResult existingOrCreated = actorManager.getOrCreate("player-1", "player")
    .inMesh("play")
    .request(new SpawnPlayer("player-1"))
    .submit()
    .toCompletableFuture().get();
```

**Options.** The same as `create` — `.inMesh(...)`, `.request(...)`, `.timeout(...)`, terminal
`.submit()` or `.yield()`.

**Completion result.** `Existing` returns the already-existing Actor and ignores `request`.
Contending with a creating attempt waits for that result and joins it; a distinct operation
receives `Existing` after Ready and does not share the earlier reply.

**When to use.** Use this when an idempotent "use if it exists, create if it doesn't" by ActorId
is needed.

---

## `find` / `findSpot` / `destroy` (manager)

Queries an existing Actor, queries the Spot it currently participates in, or terminates the exact
incarnation.

```java
Optional<ActorRef> actor = actorManager.find("player-1").toCompletableFuture().get();
Optional<SpotRef> spot = actorManager.findSpot("player-1").toCompletableFuture().get();

if (actor.isPresent()) {
    boolean destroyed = actorManager.destroy(actor.get()).toCompletableFuture().get();
}
```

**Options.** None of the three calls has modifiers — all only take the target identifier.

**Completion result.** `find` returns `Optional.empty()` if there is no Ready Actor. `findSpot`
returns `Optional.empty()` if there is no current User Spot membership. `destroy` returns `false`
if the incarnation does not exist, completes with `INVALID_OPERATION` if the generation differs,
and `UNAVAILABLE` while a pre-commit seal is in progress.

**When to use.** Use this when you need to check current existence/membership, or explicitly
terminate an Actor.

---

## `sendToActor` / `requestToActor` (ZLinkActorClient)

Sends a one-way message, or exchanges a typed request/reply, to a single global ActorId. Used
from an external client.

```java
actorClient.sendToActor("player-1", new GrantItem("sword")).submit();

CompletionStage<Inventory> reply = actorClient
    .requestToActor("player-1", new GetInventory())
    .timeout(Duration.ofSeconds(3))
    .submit(Inventory.class);
```

**Options.** `sendToActor` only has `.metadata(...)` and terminal `.submit()`. `requestToActor`
additionally has the following.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(Duration)` | The MeshNode's request default timeout | The upper bound for waiting on the reply |
| `.submit(TReply.class)` | terminal (pick one) | Waits until the reply arrives |
| `.yield(TReply.class)` | terminal (pick one) | Only valid inside a `SpotWide` handler |

**Completion result.** No ActorId completes with `NOT_FOUND`. The remaining completion kinds
follow the same common rules as the messaging-execution category.

**When to use.** Use `sendToActor` if no reply is needed, and `requestToActor` if one is.

---

## `joinSpot` / `joinEntrySpot` (inside Actor code)

Joins the current Actor to a User Spot or an Entry Spot. Called via
`ZLinkActorContext.joinSpot(...)`/`joinEntrySpot(...)` — unlike other entries, the only terminal
here is `defer()`, not `submit`/`yield`.

```java
context.joinSpot("room-42", new JoinRoomRequest("player-1"))
    .timeout(Duration.ofSeconds(5))
    .defer();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.timeout(Duration)` | 5 seconds | A monotonic absolute deadline |
| `.defer()` | Required terminal | A synchronous call with no result. Only registers the join intent and an inactive barrier — it does not start the target lookup immediately |

**Completion result.** `defer()` itself has no return value. If the current handler ends
normally, the barrier activates and executes the Join; if the handler fails, the barrier is
discarded. The actual result (accepted/rejected/failed) is delivered asynchronously via the
`ZLinkActor.onJoinCompleted(...)` callback carrying the same 128-bit
`ZLinkActorJoinOperationId` — one of `Accepted`/`Rejected`/`Failed` (the sealed interface
`ZLinkActorJoinCompletion`).

**When to use.** Use this to move an Actor to a different Spot, or return it to an Entry Spot.
Calling it from an Actor in an Entry Spot or a `PER_ACTOR` User Spot completes with
`INVALID_OPERATION`.

---

## Relocation policy selection (at Actor factory registration time)

Choose exactly one, in the `configure` callback of
`addActorFactory(actorType, actorClass, factoryClass, configure)` (topology-discovery category).

| Policy | Behavior on cross-node move | When to use |
| --- | --- | --- |
| `disableRelocation()` | Rejects the move itself before Capture | When this Actor must never be moved to another node |
| `recreateOnRelocation()` | Recreates the same logical identity via the target factory. Does not restore application state | When an Actor may be recreated without state |
| `preserveStateWith(adapterClass)` | Moves an opaque `byte[]` via `ZLinkActorRelocationAdapter<TActor>.capture`/`restore` | When state must be preserved across the move |

**Completion result.** `preserveStateWith`'s `capture(...)` result is capped at 64 MiB.
Capture/Restore can each be called multiple times within the same relocation, so both callbacks
must be retry-safe — they must not depend on an external side effect executing exactly once. If
`adapterClass` does not implement `ZLinkActorRelocationAdapter<TActor>` for that Actor type, it is
a startup configuration error.

**When to use.** Which of the three policies you choose determines this Actor type's entire
relocation behavior — it is decided once, at factory registration time, and cannot be changed
per call afterward.

---

See the
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.en.md)
(Korean-only) for the full rationale.
