# 04. Spot instance

[Reference index](README.en.md)

This category covers the external entry points `IZLinkSpotManager`·`IZLinkSpotClient`·
`IZLinkSpotPublisherClient` provide, and the entry points used inside Spot code through
`IZLinkSpotContext`. The exact signatures are owned by the
[Spot exact interface](../../common/spec/server/languages/dotnet/interfaces/05-spots.en.md)
(Korean-only).

---

## `Create`

Always creates a new User Spot. The Framework issues a new global SpotId.

```csharp
ZLinkSpotCreateResult created = await spotManager
    .Create("room")
    .InMesh("play")
    .Request(new CreateRoom("ranked"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async(ct);

string spotId = created.Spot.SpotId;
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.InMesh(meshName)` | may be omitted if the Object Client·Server role has exactly one Mesh | the Mesh to create the Spot in. Omitting it with two or more candidates completes with `InvalidOperation`; with none, `NotConfigured`; a specified Mesh that does not exist, `NotFound` |
| `.Request(ZLinkMessage)` / `.Request<TRequest>(TRequest)` | none (empty request) | the creation request passed to the Spot's `OnCreateAsync`. Bounded to 1 MiB |
| `.Timeout(TimeSpan)` | the default applied across resolve·factory·initialize as a whole | the upper bound until the entire creation reaches a terminal state |
| `.Async(ct)` | terminal (choose one, single-use) | waits until creation completes |
| `.Yield(ct)` | terminal (choose one, single-use) | valid only inside a `SpotWide` handler |

**Completion.** `ZLinkSpotCreateResult.State` is `Created` (newly created). If the Spot's
`OnCreateAsync` rejects, it is `Rejected` and `Reply` carries the rejection message. Setting the
same option twice, or calling a terminal twice, completes with `InvalidOperation`; not finishing
within the deadline completes with `DeadlineExceeded`.

**When to use it.** Use it when a new instance is always required. To reuse an existing one and
only create when absent, use `GetOrCreate`.

---

## `GetOrCreate`

Returns the Ready Spot at the given SpotId if one exists; otherwise creates a new one.

```csharp
ZLinkSpotCreateResult existingOrCreated = await spotManager
    .GetOrCreate("lobby-eu", "lobby")
    .InMesh("play")
    .Request(new CreateLobby("eu"))
    .Async(ct);
```

**Options.** Same as `Create` — `.InMesh(...)`, `.Request(...)`, `.Timeout(...)`, terminal
`.Async(ct)` or `.Yield(ct)`.

**Completion.** If `State` is `Existing`, it returns the existing Spot as-is and ignores
`Request`. `Created` means it was newly made. If the same SpotId is contested as `Creating`, it
waits for that result and joins it; if cleanup makes it Missing, it competes for a new
reservation again. If the kind or stable type differs from the existing authority, it completes
with `TypeMismatch`.

**When to use it.** Use it when an idempotent "use if present, create otherwise" by SpotId is
needed. If a new instance is always required, use `Create`.

---

## `FindAsync` / `CloseAsync` (manager)

Looks up an existing Spot, or closes its exact incarnation.

```csharp
SpotRef? spot = await spotManager.FindAsync("lobby-eu", ct);

if (spot is { } found)
{
    bool closed = await spotManager.CloseAsync(found, ct);
}
```

**Options.** Neither call has modifiers — they take only the target identifier and a
`CancellationToken`.

**Completion.** `FindAsync` returns `null` if no Ready Spot exists. `CloseAsync` returns `false`
if that incarnation does not exist, completes with `InvalidOperation` if the generation differs,
and `Unavailable` while a pre-commit seal is in progress. If a User Spot still has Actor
membership it returns `false` and does not automatically leave·destroy the Actor.

**When to use it.** Use it when a point-in-time existence check or an explicit close is needed.
`CloseAsync` never closes a different incarnation in place of a stale `SpotRef`.

---

## `SendToSpot<TMessage>`

Sends a one-way message using a single global SpotId. Use it from an external client (a
Node·Channel handler, another Actor·Spot, or application code).

```csharp
await spotClient
    .SendToSpot("room-42", new PlayerJoinedRoom("player-1"))
    .Async(ct);

// activating a new Instance Spot (cold activation) if one is needed to send to
await spotClient
    .SendToSpot("device-42", new DeviceCommand("reboot"))
    .InstanceSpot("device")
    .Async(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Metadata(...)` | none | key-value passed to the handler |
| `.InstanceSpot()` | none (resolves User Spot only) | cold-activates if Missing. The type may be omitted only when exactly one Instance Spot type is registered |
| `.InstanceSpot(instanceSpotType)` | — | must specify the type when multiple types are registered |
| `.InMesh(meshName)` | may be omitted if the Object Client·Server role has exactly one Mesh | the Mesh to first create a Missing Instance Spot in. Using it without the Instance marker completes with `InvalidOperation` |
| `.Async(ct)` | required terminal | waits only until source-local admission |

**Completion.** `NotFound` if the SpotId does not exist and no Instance marker is present.
`TypeMismatch` if `InstanceSpot(...)` was used but the existing authority is a User Spot, or
differs from the specified type. Other completion kinds match the common rules in the
messaging-execution category.

**When to use it.** Use it for Spot messaging that needs no reply. If a reply is needed, use
`RequestToSpot`.

---

## `RequestToSpot<TRequest, TResponse>`

Exchanges a typed request/reply using a single global SpotId.

```csharp
var reply = await spotClient
    .RequestToSpot("room-42", new GetRoomState())
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<RoomState>(ct);
```

**Options.** In addition to `SendToSpot`'s `.InstanceSpot(...)`/`.InMesh(...)`, this call also
has:

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | `DefaultRequestTimeout` | the deadline covering resolve, cold activation, handler, and reply as a whole |
| `.Async<TResponse>(ct)` | terminal (choose one) | waits until the reply arrives |
| `.Yield<TResponse>(ct)` | terminal (choose one) | valid only inside a `SpotWide` User Spot·Instance Spot handler. Calling it elsewhere completes with `InvalidOperation` |

**Completion.** In addition to the same failure kinds as `SendToSpot`, a factory or initialize
failure during cold activation completes as a typed failure — the Framework does not retry it
internally.

**When to use it.** Use it when a reply value is needed. For one-way, use `SendToSpot`.

---

## `Publish<TEvent>` (Spot Logical Multicast)

Publishes a typed event to subscribers by ChannelName and topic. `IZLinkSpotPublisherClient`
(external) and `IZLinkSpotOutbound` (inside Spot code, via `Context.Outbound`) provide the same
shape.

```csharp
await spotPublisherClient
    .Publish("room.events", "room-42", new RoomStateChanged("started"))
    .Async(ct);
```

**Options.** This call has only the `.Async(ct)` terminal — the topic is a required argument.

**Completion.** Normal completion means publish admission finished. It does not wait for
subscriber receipt. Unlike the messaging-execution category's classic fanout `Publish`, the
ChannelName alone determines the owner MeshNode, and the caller does not pass a MeshName
separately.

**When to use it.** Use it to notify observers of a Spot state change. If a direct reply to a
subscriber is needed, use `RequestToSpot` instead of this entry.

---

## `AddTimer<THandler>` (inside Spot code)

Registers a periodic timer that belongs to the Spot. Call it through `Context.AddTimer(...)`.

```csharp
IZLinkTimer timer = await Context.AddTimer<RoomTickHandler>(
    "room-tick",
    TimeSpan.FromSeconds(1),
    new ZLinkTimerOptions { OverrunPolicy = ZLinkTimerOverrunPolicy.SkipLateTicks },
    ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `options.OverrunPolicy` | `SkipLateTicks` | whether to skip a delayed tick, catch up within a bound, or delay the next tick |
| `options.MaxCatchUpTicks` | 1 | the maximum ticks to catch up at once under `CatchUpBounded` |
| `options.StopOnUnhandledException` | `false` | whether the timer stops when the handler throws |

**Completion.** Returns an `IZLinkTimer`. Because the timer is a logical registration that
belongs to this Spot, it moves automatically on relocation and the application does not need to
re-register it at the target. Cancel it with `CancelAsync()` or `DisposeAsync()`.

**When to use it.** Use it when a Spot needs periodic work.

---

## `RunCpuWorker<TResult>` / `RunIoWorker<TResult>` (inside Spot code)

Runs work on a separate worker without blocking the Spot's owner turn.

```csharp
int result = await Context
    .RunCpuWorker(ct => ComputeExpensiveScore(ct))
    .Timeout(TimeSpan.FromSeconds(2))
    .Async(ct);
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | the Worker option's default | the upper bound for the work to complete |
| `.Submit(ct)` | — | submits without waiting for the result |
| `.Async(ct)` | terminal (choose one) | waits until completion |
| `.Yield(ct)` | terminal (choose one) | valid only inside a `SpotWide` handler |

**Completion.** Returns `TResult`, or completes with `DeadlineExceeded` on timeout. The worker
pool size (`MinThreads`/`MaxThreads`) and idle timeout are set only before the host starts.

**When to use it.** Use `RunCpuWorker` for CPU-bound computation and `RunIoWorker` for work that
waits on I/O. Both exist so the owner turn's sequential execution is not blocked.

---

## Handler registration (inside Spot code, `Configure()`)

Registers the handler types that process the packets·requests·subscriptions·member-Actor
messages a Spot receives. Call it through `Context.Handlers` (`IZLinkSpotHandlerRegistry` for a
User·Entry Spot, `IZLinkInstanceSpotHandlerRegistry` for an Instance Spot), only inside the
`Configure()` override.

```csharp
public void Configure()
{
    Context.Handlers.AddPacket<ChatHandler>();               // in front of the Spot: packet·request
    Context.Handlers.AddActorPacket<JoinGameHandler, PlayerActor>(); // in front of a member Actor
    Context.Handlers.AddSubscribe<ScoreHandler>("game.scores", "world"); // a subscription event
}
```

**Options.** The registration method depends on the interface the handler implements.

| Target | Handler interface | Registration method |
| --- | --- | --- |
| One-way packet in front of a User Spot | `IZLinkSpotPacketHandler<TSpot, TMessage>` | `AddPacket<THandler>()` |
| Request in front of a User Spot | `IZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `AddPacket<THandler>()` |
| Logical Multicast subscription event | `IZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `AddSubscribe<THandler>(channelName, topic)` |
| One-way packet in front of a User Spot's member Actor | `IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `AddActorPacket<THandler, TActor>()` |
| Request in front of a User Spot's member Actor | `IZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `AddActorPacket<THandler, TActor>()` |
| The Entry Spot's own packet (before Actor binding) | `IZLinkSpotPacketHandler<TEntrySpot, TMessage>`/`IZLinkSpotRequestHandler<TEntrySpot, TRequest, TReply>` | `AddHandler<THandler>()` (inherited from the base `IZLinkActorHandlerRegistry`) |
| One-way packet in front of an Entry Spot's member Actor | `IZLinkEntrySpotActorSendHandler<TEntrySpot, TActor, TMessage>` | `AddActorPacket<THandler, TActor>()` |
| Request in front of an Entry Spot's member Actor | `IZLinkEntrySpotActorRequestHandler<TEntrySpot, TActor, TRequest, TReply>` | `AddActorPacket<THandler, TActor>()` |
| Packet in front of an Instance Spot | the same shape as `IZLinkSpotPacketHandler<TSpot, TMessage>` | `AddPacket<THandler>()` (`IZLinkInstanceSpotHandlerRegistry`) |

An Entry Spot's `AddActorPacket<THandler, TActor>()` shares its method name with a User Spot's,
but the interface `THandler` must implement differs — it depends on which Spot kind's registry
you call it through.

**Completion.** Registers synchronously with no return value. If the packet name is omitted, it
checks the `ZLinkPacketAttribute` on the handled message type, and falls back to the type name if
that is also absent. A duplicate handler key within the same owner surfaces as
`ZLinkConfigurationException` during host startup validation.

**When to use it.** Register every handler this Spot processes each time `Configure()` is
called. For Node·Channel handlers, see the topology-discovery category's registration entries;
for STREAM session handlers, see the stream-session category.

---

## `SendToChannel<TMessage>` / `RequestToChannel<TRequest, TResponse>` (inside Spot code, `Context.Outbound`)

Sends a one-way message by ChannelName, or exchanges a typed request/reply, from inside Spot
code. `IZLinkSpotOutbound` provides it, in the same shape as the messaging-execution category's
`SendToChannel`/`RequestToChannel`.

```csharp
await Context.Outbound
    .RequestToChannel<GetLeaderboard>("leaderboard.api", new GetLeaderboard())
    .Async<Leaderboard>(ct);
```

**Options.** It takes the same modifiers as the messaging-execution category's
`SendToChannel`/`RequestToChannel` — `.Metadata(...)`, `.Timeout(...)`, terminal
`.Async(ct)`/`.Async<TResponse>(ct)`.

**Completion.** Same completion kinds as the messaging-execution category.

**When to use it.** Use it when the Spot itself, rather than an external client, needs to call
another ChannelName's handler from within its own code. To call another Spot directly, use
`SendToSpot`/`RequestToSpot`.

---

## `LeaveActorAsync` / `CloseAsync` / `DestroyActorAsync` (inside Spot code, termination·departure)

Removes a member Actor from this Spot, closes the Spot itself, or destroys an Actor from the
Entry Spot.

```csharp
await Context.LeaveActorAsync(actor, ct);       // User Spot: removes only the member Actor
bool closed = await Context.CloseAsync(ct);     // User·Instance Spot: closes this Spot itself
await entryContext.DestroyActorAsync(actor, ct); // Entry Spot: destroys the Actor entirely
```

**Options.** None of the three calls have modifiers — they take only the target
(`LeaveActorAsync`/`DestroyActorAsync`) and a `CancellationToken`.

**Completion.** `LeaveActorAsync` (`IZLinkSpotContext` only) releases only the member Actor
membership and does not destroy the Actor itself. `CloseAsync`
(`IZLinkSpotContext`/`IZLinkInstanceSpotContext`) uses the same completion kinds as the
manager's `CloseAsync(spotRef)` (the earlier entry in this category), targeting this Spot
itself. `DestroyActorAsync` (`IZLinkEntrySpotContext` only) destroys the Actor entirely —
unlike `LeaveActorAsync`, it does not just release membership, it removes the Actor itself.

**When to use it.** Use `LeaveActorAsync` to remove a member Actor from this Spot without
moving it elsewhere, `CloseAsync` to close the Spot itself, and `DestroyActorAsync` to
permanently remove an Actor the Entry Spot no longer needs.

---

## `RelocationReady().Defer()` (inside Spot code)

In a `SpotWide` Spot that has selected `ApplicationSignaled` readiness mode, defers the
relocation boundary to just before the next application turn.

```csharp
Context.RelocationReady().Defer();
```

**Options.** This call has no modifiers.

**Completion.** No return value. It registers the relocation boundary after the current handler
finishes. If it did not move, or aborted before commit, the source receives `Continued`; if it
moved, the target receives `Relocated` — both delivered as completions to
`OnRelocationReadyCompletedAsync(...)`. Calling it under `AnyTurnBoundary` mode, on a `PerActor`
Spot, on an Entry·Instance Spot, outside a Spot turn, or twice within the same turn all complete
with `InvalidOperation`.

**When to use it.** Use it when the application needs to control the relocation moment precisely
at a specific turn boundary. Under the default `AnyTurnBoundary` mode, this call is not needed.

---

The full basis is the
[Spot exact interface](../../common/spec/server/languages/dotnet/interfaces/05-spots.en.md)
(Korean-only).
