# 05. Actor relocation

[Reference index](README.en.md)

This category covers the external entry points `IZLinkActorManager`·`IZLinkActorClient`
provide, the entry points used inside Actor code through `IZLinkActorContext` to join a Spot, and
relocation-policy selection. The exact signatures are owned by the
[Actor exact interface](../../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md)
(Korean-only).

---

## `Create` (Actor)

Always creates a new Actor.

```csharp
ZLinkActorCreateResult created = await actorManager
    .Create("player-1", "player")
    .InMesh("play")
    .Request(new SpawnPlayer("player-1"))
    .Async(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.InMesh(meshName)` | may be omitted if the Object Client·Server role has exactly one Mesh | the Mesh to create the Actor in. Omitting it with two or more candidates completes with `InvalidOperation`; with none, `NotConfigured`; a specified Mesh that does not exist, `NotFound` |
| `.Request(ZLinkMessage)` / `.Request<TRequest>(TRequest)` | none (empty request) | the request passed at the Actor factory's creation time. Bounded to 1 MiB |
| `.Timeout(TimeSpan)` | 5 seconds | the deadline covering resolve·reservation·factory·the `Ready` barrier as a whole |
| `.Async(ct)` | terminal (choose one, single-use) | waits until creation completes |
| `.Yield(ct)` | terminal (choose one, single-use) | valid only inside a `SpotWide` handler |

**Completion.** `ZLinkActorCreateResult` completes as one of `Created` (newly created) or
`Rejected` (the factory rejected it). If a Ready incarnation of the same ActorId already
exists, it completes as an `AlreadyExists` error instead of `Created`/`Rejected` — the
`Existing` result exists only on `GetOrCreate`. If a Ready incarnation exists but the stable
type differs, it is `TypeMismatch`.

**When to use it.** Use it when a new Actor is always required. To reuse an existing one and
only create when absent, use `GetOrCreate`.

---

## `GetOrCreate` (Actor)

Returns the Ready Actor at the same ActorId if one exists; otherwise creates a new one.

```csharp
ZLinkActorCreateResult existingOrCreated = await actorManager
    .GetOrCreate("player-1", "player")
    .InMesh("play")
    .Request(new SpawnPlayer("player-1"))
    .Async(ct);
```

**Options.** Same as `Create` — `.InMesh(...)`, `.Request(...)`, `.Timeout(...)`, terminal
`.Async(ct)` or `.Yield(ct)`.

**Completion.** `Existing` returns the existing Actor and ignores `Request`. If it contends with
a Creating attempt, it waits for that result and joins it; separate operations receive
`Existing` after Ready and do not share an earlier reply.

**When to use it.** Use it when an idempotent "use if present, create otherwise" by ActorId is
needed.

---

## `FindAsync` / `FindSpotAsync` / `DestroyAsync` (manager)

Looks up an existing Actor, looks up the Spot it is a member of, or terminates its exact
incarnation.

```csharp
ActorRef? actor = await actorManager.FindAsync("player-1", ct);
SpotRef? spot = await actorManager.FindSpotAsync("player-1", ct);

if (actor is { } found)
{
    bool destroyed = await actorManager.DestroyAsync(found, ct);
}
```

**Options.** None of the three calls have modifiers — they take only the target identifier and
a `CancellationToken`.

**Completion.** `FindAsync` returns `null` if no Ready Actor exists. `FindSpotAsync` returns
`null` if there is no current User Spot membership. `DestroyAsync` returns `false` if that
incarnation does not exist, completes with `InvalidOperation` if the generation differs, and
`Unavailable` while a pre-commit seal is in progress.

**When to use it.** Use it when a point-in-time existence·membership check or an explicit
termination is needed.

---

## `SendToActor<TMessage>` / `RequestToActor<TRequest, TResponse>`

Sends a one-way message, or exchanges a typed request/reply, using a single global ActorId. Use
it from an external client.

```csharp
await actorClient
    .SendToActor("player-1", new GrantItem("sword"))
    .Async(ct);

var reply = await actorClient
    .RequestToActor("player-1", new GetInventory())
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<Inventory>(ct);
```

**Options.** `SendToActor` has only `.Metadata(...)` and the terminal `.Async(ct)`.
`RequestToActor` also has the following.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | `DefaultRequestTimeout` | the upper bound for waiting on the reply |
| `.Async<TResponse>(ct)` | terminal (choose one) | waits until the reply arrives |
| `.Yield<TResponse>(ct)` | terminal (choose one) | valid only inside a `SpotWide` handler |

**Completion.** `NotFound` if the ActorId does not exist. The remaining completion kinds match
the common rules in the messaging-execution category.

**When to use it.** Use `SendToActor` when no reply is needed, `RequestToActor` when one is.

---

## `JoinSpot` / `JoinEntrySpot` (inside Actor code)

Makes the current Actor join a User Spot or return to the Entry Spot. Call it through
`Context.JoinSpot(...)`/`Context.JoinEntrySpot(...)`. Unlike other entries, its terminal is not
`Async`/`Yield` — it is `Defer()` alone.

```csharp
Context
    .JoinSpot("room-42", new JoinRoomRequest("player-1"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Defer();
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | 5 seconds | a monotonic absolute deadline |
| `.Defer()` | required terminal | a synchronous call with no return value. It registers the Join intent and an inactive barrier only, and does not start the target lookup immediately |

**Completion.** `Defer()` itself has no return value. If the current handler finishes normally,
the barrier activates and executes the Join; if the handler fails, the barrier is discarded. The
actual result (accepted·rejected·failed) is delivered asynchronously through the
`OnJoinCompletedAsync(ZLinkActorJoinCompletion, ct)` callback carrying the same
`ZLinkActorJoinOperationId` — it is one of `Accepted`/`Rejected`/`Failed`.

**When to use it.** Use it to move an Actor to a different Spot, or return it to the Entry Spot.
Calling it from an Entry Spot's Actor or a `PerActor` User Spot's Actor completes with
`InvalidOperation`.

---

## Relocation-policy selection (Actor factory registration time)

Choose exactly one in the `configure` callback of `AddActorFactory<TActor, TFactory>(...)`
(topology-discovery category).

| Policy | Behavior on cross-node move | When to use it |
| --- | --- | --- |
| `DisableRelocation()` | Rejects the move itself before capture | when this Actor must never move to another node |
| `RecreateOnRelocation()` | Recreates the same logical identity with the target factory. It does not recover application state | when it is fine to recreate the Actor without state |
| `PreserveStateWith<TAdapter>()` | Moves an opaque byte array via `IZLinkActorRelocationAdapter<TActor>.CaptureAsync`/`RestoreAsync` | when state must be preserved across the move |

**Completion.** `PreserveStateWith`'s `CaptureAsync` result is bounded to 64 MiB. Because
Capture·Restore can each be called more than once for the same relocation, both callbacks must
be retry-safe — they must not depend on an external side effect executing exactly once.

**When to use it.** Which of the three policies is chosen decides this Actor type's entire
relocation behavior — it is set once at factory registration time and cannot change per call
afterward.

---

The full basis is the
[Actor exact interface](../../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md)
(Korean-only).
