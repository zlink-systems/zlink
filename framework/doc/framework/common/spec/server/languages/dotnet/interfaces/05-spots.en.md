# .NET Spot Public Interface

A Spot relocation, including an Actor bound to a session, keeps the same
`ObjectGeneration`. Since relocation itself isn't a physical/logical
disconnect, it doesn't run the Actor disconnect callback.

[.NET exact interface table of contents](README.en.md)

## 1. Spot

SpotId is a `string` with UTF-8 encoded size 1..255 bytes, and is a
logical ID unique across the whole Location Store transaction domain.
Comparison is case-sensitive exact match, with no normalization. A
regular message only takes SpotId and resolves current authority.
`SpotRef` is the immutable location snapshot used when closing an exact
incarnation, and doesn't own a runtime resource or local Spot instance.

```csharp
public enum ZLinkSpotKind
{
    Invalid = 0,
    Entry = 1,
    User = 2,
    Instance = 3
}

public readonly record struct SpotRef(
    string SpotId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId NodeRid);

public enum ZLinkSpotCloseReason
{
    ExplicitClose = 0,
    HostShutdown = 1,
    RelocationOut = 2,
    IdleEvicted = 3
}

public readonly record struct ZLinkSpotClosingContext(
    ZLinkSpotCloseReason Reason,
    DateTimeOffset Deadline);

public enum ZLinkSpotRelocationReadyOutcome
{
    Continued = 0,
    Relocated = 1
}

public readonly record struct ZLinkSpotRelocationReadyCompletion(
    ZLinkSpotRelocationReadyOutcome Outcome);

public interface IZLinkSpotRelocationReadyCall
{
    void Defer();
}

public interface IZLinkSpot
{
    IZLinkSpotContext Context { get; }
    void Configure()
    {
    }

    ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        // the default lifecycle accepts the creation request.
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnRelocationReadyCompletedAsync(
        ZLinkSpotRelocationReadyCompletion completion,
        CancellationToken cancellationToken)
    {
        // override only when the application needs round-boundary follow-up processing.
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkInstanceSpot
{
    IZLinkInstanceSpotContext Context { get; }
    void Configure()
    {
    }

    ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkSpotRelocationAdapter<TSpot>
    where TSpot : class
{
    ValueTask<byte[]> CaptureAsync(
        TSpot spot,
        CancellationToken cancellationToken);
    ValueTask RestoreAsync(
        TSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken);
}

public readonly record struct ZLinkSpotCreateResponse(
    bool Accepted,
    ZLinkMessage? Reply)
{
    public static ZLinkSpotCreateResponse Accept(ZLinkMessage? reply = null);
    public static ZLinkSpotCreateResponse Accept<TReply>(TReply reply);
    public static ZLinkSpotCreateResponse Reject(ZLinkMessage? reply = null);
    public static ZLinkSpotCreateResponse Reject<TReply>(TReply reply);
}

public interface IZLinkSpotHandlerRegistry : IZLinkActorHandlerRegistry
{
    void AddPacket<THandler>() where THandler : class;
    void AddSubscribe<THandler>(string channelName, string topic) where THandler : class;
}

public interface IZLinkInstanceSpotHandlerRegistry
{
    void AddPacket<THandler>() where THandler : class;
}

public interface IZLinkSpotOutbound
{
    IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message);
    IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request);
    IZLinkPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);
    IZLinkSendCall SendToChannel<TMessage>(
        string channelName,
        TMessage message);
    IZLinkRequestCall RequestToChannel<TRequest>(
        string channelName,
        TRequest request);
}

public interface IZLinkSpotCommonContext
{
    string MeshName { get; }
    string SpotId { get; }
    ulong ObjectGeneration { get; }
    RoutingId NodeRid { get; }
    IZLinkSpotOutbound Outbound { get; }
    ValueTask<IZLinkTimer> AddTimer<THandler>(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options = null,
        CancellationToken cancellationToken = default)
        where THandler : class;
    IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
        Func<CancellationToken, TResult> work);
    IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
        Func<CancellationToken, ValueTask<TResult>> work);
}

public interface IZLinkSpotContext : IZLinkSpotCommonContext
{
    IZLinkSpotHandlerRegistry Handlers { get; }

    IZLinkSpotRelocationReadyCall RelocationReady();

    ValueTask LeaveActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default);

    ValueTask<bool> CloseAsync(
        CancellationToken cancellationToken = default);
}

public interface IZLinkInstanceSpotContext : IZLinkSpotCommonContext
{
    IZLinkInstanceSpotHandlerRegistry Handlers { get; }

    ValueTask<bool> CloseAsync(
        CancellationToken cancellationToken = default);
}

public interface IZLinkEntrySpot
{
    IZLinkEntrySpotContext Context { get; }
    void Configure()
    {
    }

    ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkSpotActorMembershipLifecycle<TActor>
    where TActor : IZLinkActor
{
    ValueTask OnJoinedActorAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask OnLeaveActorAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask OnDisconnectActorAsync(
        TActor actor,
        CancellationToken cancellationToken)
    {
        // a Spot that doesn't need to handle disconnects doesn't have to implement this callback.
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkUserSpotActorLifecycle<TActor>
    : IZLinkSpotActorMembershipLifecycle<TActor>
    where TActor : IZLinkActor
{
    ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken);
}

public interface IZLinkSpot<TActor> : IZLinkSpot, IZLinkUserSpotActorLifecycle<TActor>
    where TActor : IZLinkActor;

public readonly record struct ZLinkActorCreateResponse(
    bool Accepted,
    ZLinkMessage? Reply)
{
    public static ZLinkActorCreateResponse Accept(ZLinkMessage? reply = null);
    public static ZLinkActorCreateResponse Accept<TReply>(TReply reply);
    public static ZLinkActorCreateResponse Reject(ZLinkMessage? reply = null);
    public static ZLinkActorCreateResponse Reject<TReply>(TReply reply);
}

public interface IZLinkEntrySpot<TActor>
    : IZLinkEntrySpot, IZLinkSpotActorMembershipLifecycle<TActor>
    where TActor : IZLinkActor
{
    ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        TActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        // the default lifecycle approves Actor creation.
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }
}

public readonly record struct ZLinkSpotActorJoinResult(
    bool Accepted,
    ZLinkMessage? Reply)
{
    public static ZLinkSpotActorJoinResult Accept(ZLinkMessage? reply = null);
    public static ZLinkSpotActorJoinResult Accept<TReply>(TReply reply);
    public static ZLinkSpotActorJoinResult Reject(ZLinkMessage? reply = null);
    public static ZLinkSpotActorJoinResult Reject<TReply>(TReply reply);
}

public interface IZLinkEntrySpotContext : IZLinkSpotCommonContext
{
    IZLinkSpotHandlerRegistry Handlers { get; }

    ValueTask DestroyActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotActorSendHandler<TSpot, TActor, in TMessage>
    where TSpot : class
    where TActor : IZLinkActor
{
    ValueTask HandleAsync(
        TSpot spot,
        TActor actor,
        IZLinkMessageContext context,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotActorRequestHandler<TSpot, TActor, in TRequest, TReply>
    where TSpot : class
    where TActor : IZLinkActor
{
    ValueTask<TReply> HandleAsync(
        TSpot spot,
        TActor actor,
        IZLinkMessageContext context,
        TRequest request,
        CancellationToken cancellationToken);
}

public interface IZLinkEntrySpotActorSendHandler<TEntrySpot, TActor, in TMessage>
    where TEntrySpot : class, IZLinkEntrySpot
    where TActor : IZLinkActor
{
    ValueTask HandleAsync(
        TEntrySpot entrySpot,
        TActor actor,
        IZLinkMessageContext context,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkEntrySpotActorRequestHandler<TEntrySpot, TActor, in TRequest, TReply>
    where TEntrySpot : class, IZLinkEntrySpot
    where TActor : IZLinkActor
{
    ValueTask<TReply> HandleAsync(
        TEntrySpot entrySpot,
        TActor actor,
        IZLinkMessageContext context,
        TRequest request,
        CancellationToken cancellationToken);
}
```

The framework creates a Spot's packet/request/subscription/timer handler
once in that Spot's activation DI scope, and reuses it for the duration
of the Spot activation. An Actor send/request handler is created once in
that Actor's separate activation DI scope, and reused for the duration of
the Actor activation. Different Actors of an Entry Spot and a `PerActor`
User Spot don't share a handler instance or scoped dependency.

The handler type's own DI registration lifetime doesn't change this
rule. The framework owns the handler instance and only resolves
constructor dependencies in that activation scope. A separate handler
lifetime option isn't provided. On Spot/Actor relocation and cross-node
Join, the source handler and scope are cleaned up and re-created in the
target activation. The state that must be recovered is owned by `TSpot`
or `TActor`, not a handler field.

`ZLinkSpotCloseReason`'s numeric values are `ExplicitClose=0`,
`HostShutdown=1`, `RelocationOut=2`, `IdleEvicted=3`. `IdleEvicted` is an
Instance-Spot-only reason and isn't delivered to Entry Spot or User Spot.
The idle judgment condition and the reactivation rule after cleanup are
owned by
[Spot Model §6.2](../../../../11-spot-model.en.md#62-cleaning-up-an-idle-instance-spot).
`Deadline` is the closing operation's absolute deadline. The framework
doesn't cancel `cleanupCancellationToken` before the callback invocation,
and cancels it when the
[deadline](../../../../01-glossary.en.md#deadline) ends. An already
cancelled handler token isn't reused. Only Entry/User/Instance Spot
receive this callback — a per-Actor closing callback isn't provided.
Host Shutdown runs the callback while Actor membership and the local
instance are valid, and cleans up scope and
[authority](../../../../01-glossary.en.md#authority) after completion.
A standalone Actor relocation doesn't close the Entry Spot, so it doesn't
call this callback.

`IZLinkSpotRelocationAdapter<TSpot>` is registered with
`PreserveStateWith<TAdapter>()`. It's only called when materializing a
cross-node User/[Instance Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
instance. In a whole User Spot relocation, the
[Spot](../../../../01-glossary.en.md#spot) adapter handles the Spot
application payload, and each member Actor's payload is handled
separately by the Actor adapter registered on the Actor factory.
`RecreateOnRelocation()` doesn't call the adapter and re-creates the
instance with no application state, and `DisableRelocation()` rejects a
cross-node move before capture.

The Spot adapter's capture and restore can be called at-least-once within
a stable relocation attempt, so they must be retry-safe.
`CaptureAsync(...)`'s result is at most 64 MiB; an empty array is valid,
and null is a contract violation. The framework immediately copies the
completed array and doesn't observe subsequent application mutation.
`RestoreAsync(...)`'s `ReadOnlyMemory<byte>` is only valid until the
callback completes, so the application must copy it to keep it. A capture
exception restores admission after a durable abort and source
normalization. An instance with a restore exception is discarded, and a
new attempt applies the same immutable payload to a new instance the
[factory](../../../../01-glossary.en.md#factory) creates. If the
framework cancels a callback due to the operation deadline, it's
classified as `DeadlineExceeded`. The framework doesn't guarantee
exactly-once execution of the callback's external side effect.

When maintenance restores an Actor to a different node's Entry Spot, it
first finishes the Actor adapter restore, then commits Location
authority and Entry [membership](../../../../01-glossary.en.md#membership).
Since this isn't an application membership change, it doesn't call the
target's `OnJoinedActorAsync(...)`, the source's
`OnLeaveActorAsync(...)`, or a relocation-dedicated callback. It restores
the accepted journal/queue/Actor timer, commits Location authority/
membership, and then starts Actor message processing. The Bound Session
location update is then performed with `sessionActorLocationUpdateReqMsg`
and `sessionActorLocationUpdateResMsg` send messages, and Actor
processing doesn't stop even without a response. A regular application
join toward a User Spot keeps the order: target's
`OnActorJoinAsync(...)`, membership commit, target's
`OnJoinedActorAsync(...)`. An Entry Spot return commits membership with
no admission callback and then calls the target Entry Spot's
`OnJoinedActorAsync(...)`. `SpotWide` User Spot aggregate move and a
`PerActor` User Spot's Actor relocation also don't call an application
membership callback.

A `PerActor` User Spot only allows the `RecreateOnRelocation` Spot
policy and doesn't register a Spot relocation adapter. Spot fields and a
Spot-level application timer aren't relocation targets. It moves the
Actor independently after first switching the target Spot authority —
`ToSpot`/Create/Join use Spot authority, and `ToActor` uses the current
owner per Actor. The target's runtime-private shell uses the same public
SpotId and ObjectGeneration, and isn't exposed to public lookup before
the authority switch. A stale source route is relayed while preserving
operation identity, generation, deadline, correlation, and reply route.
The 1-second window from Actor queue seal to target admission is an
operational goal — exceeding it doesn't cancel or roll back the
relocation.

`RelocationReady().Defer()` is only valid on a Spot turn where the
`SpotWide` factory selected the `ApplicationSignaled` readiness mode.
`Defer()` registers a relocation boundary right before the next
application turn, after the current handler finishes. The framework
delivers `Continued` from the source if it didn't move or aborted before
commit, and `Relocated` from the target if it moved, to
`OnRelocationReadyCompletedAsync(...)`'s completion. The default
implementation is a no-op. Held messages and timers aren't run before the
callback completes.

A duplicate `Defer()` on the default `AnyTurnBoundary`, on `PerActor`, on
Entry/Instance Spot, outside the Spot turn, or in the same turn ends with
a `ZLinkFrameworkErrorKind.InvalidOperation` error before any queue
mutation. Starting a different framework operation in the same turn
after `Defer()` is the same error. Since the callback can run again
during process recovery, the override must be retry-safe.

The current-location query for Spot and Actor is performed by the
manager using the global ID. A public resolver and runtime handle aren't
provided. The owner route and generation update rule follows
[Spot Address Messaging](../../../../16-spot-address-messaging.en.md).

The Spot handler signatures are as follows.

```csharp
public interface IZLinkSpotPacketHandler<TSpot, in TMessage>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotRequestHandler<TSpot, in TRequest, TReply>
    where TSpot : class
{
    ValueTask<TReply> HandleAsync(
        TSpot spot,
        TRequest request,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotSubscriptionHandler<TSpot, in TEvent>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        TEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotTimerHandler<TSpot>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken);
}

public interface IZLinkTimer : IAsyncDisposable
{
    bool IsDisposed { get; }
    ValueTask CancelAsync();
}

public sealed record ZLinkTimerOptions
{
    public ZLinkTimerOverrunPolicy OverrunPolicy { get; init; }
        = ZLinkTimerOverrunPolicy.SkipLateTicks;
    public int MaxCatchUpTicks { get; init; } = 1;
    public bool StopOnUnhandledException { get; init; }
}

public enum ZLinkTimerOverrunPolicy
{
    SkipLateTicks = 1,
    CatchUpBounded = 2,
    DelayNextTick = 3
}

public readonly record struct ZLinkTimerTick(
    string Name,
    ulong DeliveryIndex,
    ulong ScheduledIndex,
    TimeSpan Period,
    DateTimeOffset ScheduledAt,
    DateTimeOffset StartedAt,
    TimeSpan ScheduledElapsed,
    TimeSpan StartedElapsed,
    TimeSpan Delay,
    ulong SkippedTicks);
```

A Framework timer is a logical registration belonging to the owner
Actor/Spot. On cross-node relocation, the timer name, handler type,
period, `ZLinkTimerOptions`, scheduling cursor, and the pending tick at
seal time are automatically included in the relocation payload. The
application's relocation adapter doesn't capture/restore the timer or
re-register it on the target. A framework-managed timer resource isn't
included in the payload — it's re-created on the target as a logical
registration. The source doesn't dispatch a new tick after sealing the
queue, and the target only submits the restored pending tick and the
next tick to the [owner](../../../../01-glossary.en.md#owner) mailbox
after finishing Restore and authority commit and dispatch admission
opens.

An external client of a Spot uses the following signatures.

```csharp
public interface IZLinkSpotClient
{
    IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message);
    IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request);
}

public interface IZLinkSpotSendCall : IZLinkMetadataCall<IZLinkSpotSendCall>
{
    IZLinkSpotSendCall InstanceSpot();
    IZLinkSpotSendCall InstanceSpot(string instanceSpotType);
    IZLinkSpotSendCall InMesh(string meshName);
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotRequestCall : IZLinkMetadataCall<IZLinkSpotRequestCall>
{
    IZLinkSpotRequestCall InstanceSpot();
    IZLinkSpotRequestCall InstanceSpot(string instanceSpotType);
    IZLinkSpotRequestCall InMesh(string meshName);
    IZLinkSpotRequestCall Timeout(TimeSpan timeout);
    ValueTask<TReply> Async<TReply>(
        CancellationToken cancellationToken = default);
    ValueTask<TReply> Yield<TReply>(
        CancellationToken cancellationToken = default);
}

public enum ZLinkSpotCreateState
{
    Existing = 0,
    Created = 1,
    Rejected = 2
}

public readonly record struct ZLinkSpotCreateResult(
    SpotRef Spot,
    ZLinkSpotCreateState State,
    ZLinkMessage? Reply);

public interface IZLinkSpotManager
{
    IZLinkSpotCreateCall Create(string spotType);
    IZLinkSpotGetOrCreateCall GetOrCreate(
        string spotId,
        string spotType);
    ValueTask<SpotRef?> FindAsync(
        string spotId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> CloseAsync(
        SpotRef spot,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotCreateCall
{
    IZLinkSpotCreateCall InMesh(string meshName);
    IZLinkSpotCreateCall Request(ZLinkMessage request);
    IZLinkSpotCreateCall Request<TRequest>(TRequest request);
    IZLinkSpotCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkSpotCreateResult> Async(CancellationToken cancellationToken = default);
    ValueTask<ZLinkSpotCreateResult> Yield(CancellationToken cancellationToken = default);
}

public interface IZLinkSpotGetOrCreateCall
{
    IZLinkSpotGetOrCreateCall InMesh(string meshName);
    IZLinkSpotGetOrCreateCall Request(ZLinkMessage request);
    IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request);
    IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkSpotCreateResult> Async(CancellationToken cancellationToken = default);
    ValueTask<ZLinkSpotCreateResult> Yield(CancellationToken cancellationToken = default);
}

public interface IZLinkSpotPublisherClient
{
    IZLinkPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);
}
```

Entry/User/Instance SpotId is a global string key with UTF-8 encoded
size 1..255 bytes. Stable type is UTF-8 1..255 bytes, compared as a
case-sensitive exact value with no normalization. `SpotRef.ObjectGeneration`
is `1..long.MaxValue`. MeshName and NodeRid are the route snapshot at
query time and aren't included in the identity key.

`IZLinkSpotOutbound` and `IZLinkSpotClient` take the global SpotId and
return a Spot-dedicated call. A regular call only resolves the current
Ready location. If SpotId doesn't exist, both send and request complete
with `NotFound`. Only a call with `InstanceSpot()` or
`InstanceSpot(instanceSpotType)` specified can newly create and
initialize a Missing Instance Spot to prepare it for use. This process is
called cold activation. A call to a
[Ready](../../../../01-glossary.en.md#ready) Instance authority uses the
[stable type](../../../../01-glossary.en.md#stable-type) stored in
authority, so the caller doesn't need to provide the type again. If an
Instance marker is used but the existing authority is a User Spot, or the
specified stable type differs from the authority's type, it's
`TypeMismatch`.

In [cold activation](../../../../01-glossary.en.md#cold-activation),
`InstanceSpot()` only uses the registered Instance Spot type when exactly
one is registered on the selected Mesh. If multiple types are
registered, the type must be specified with
`InstanceSpot(instanceSpotType)`. If the selected Mesh has no type, it's
`NotFound`; if it has multiple and the type is omitted, it completes
with `InvalidOperation`. `InMesh` specifies the Mesh to first create the
Missing Instance Spot on. It can be omitted if there's one Object Client
or Server role Mesh. If there are two or more candidates and it's
omitted, `InvalidOperation`; if there are no candidates, `NotConfigured`;
if the specified Mesh doesn't exist, it completes with `NotFound`. This
option is only valid on a call with the Instance marker — using it
without the marker is `InvalidOperation`. It doesn't move an existing
authority to a different Mesh or owner. Even with this option, a target
node or endpoint can't be specified. Request `Timeout` fixes the deadline
across resolve, cold activation, handler, and reply as a whole. A
one-way call includes resolve, cold activation, and outbound admission
within the selected MeshNode's send deadline. Metadata and terminal
ownership are each kept by the call, and send is submitted once with
`Async`, and request with `Async<TReply>` or `Yield<TReply>`. The
Instance marker and each option can only be set once. `Yield<TReply>` is
only valid in a `SpotWide` User Spot or Instance Spot callback. Calling
it on an Entry Spot, `PerActor` User Spot, Entry Spot Actor, Node/Channel
handler, or a client outside the owner turn completes with
`InvalidOperation`, without submitting the operation or returning the
turn.

`IZLinkInstanceSpot` is an actor-free lifecycle interface that doesn't
inherit `IZLinkSpot`. It can only register a direct packet and timer
handler. If an Actor handler or Logical Multicast subscription is
registered, the framework rejects activation before the `Ready` commit.

A store-backed User Spot is created through the manager's create
operation. An Instance Spot starts cold activation from the first
message with an explicit `InstanceSpot(...)` intent. If an existing
`Ready` Spot exists, the same call is delivered to the current owner. If
cold activation is needed, the framework selects an eligible target and,
once initialization finishes, processes the first message exactly once
as that Spot's first job. An API for the caller to directly specify or
control the target node, activation driver, or internal reservation isn't
provided.

The source doesn't secure an owner claim or admission room in advance.
Only when the target has no local instance of the same Spot does it
secure, together, a `Creating` record and admission room with itself as
owner. Only the one target that succeeds this runs factory and
initialization.

`CloseAsync(spotRef)` only closes the exact incarnation. If that
incarnation doesn't exist, `false`; if the generation differs,
`InvalidOperation`; if in pre-commit seal, `Unavailable`. If Actor
membership remains on the User Spot, `false` — it doesn't automatically
leave/destroy the Actor. The framework doesn't find the current ref again
and close a different incarnation.

`IZLinkSpotManager` only provides User Spot's explicit create/
get-or-create, resolve, and exact close. The manager doesn't have an
argument to select Spot kind or an Instance Spot create/get-or-create
overload. Instance Spot's creation path is the one explicit
`InstanceSpot(...)` opt-in on the Spot-dedicated message call. It leaves
`IZLinkInstanceSpotContext.CloseAsync()` for an Instance Spot
implementation to close its own lifecycle.

User Spot Create and GetOrCreate calls are single-use. Setting the same
option twice is `InvalidOperation`, and calling terminal `Async(...)`
twice is `InvalidOperation`. The `InMesh(...)` selection and error and
whole-deadline rules are the same as Actor create. `Create` has the
framework issue a new global Spot ID. `GetOrCreate` returns a Ready Spot
of the same User Spot stable type as `Existing`. If Creating, it waits
for the authority change; once Ready, `Existing`; if it becomes Missing
through cleanup, it competes for a new reservation. A CAS loser doesn't
run a separate factory. If kind or type differs, `TypeMismatch`; if the
terminal state isn't reached within the deadline, `DeadlineExceeded`. A
creation request is at most 1 MiB and is kept as an immutable reference
and hash before reservation.

The first `InstanceSpot(...)` call against Missing authority records
kind, stable type, and initial Mesh in the creation intent. A message
against Ready authority resolves the current owner using only SpotId.
Reactivation after owner loss uses the intent stored in authority, and a
Missing call with no marker doesn't create a new intent. A public
activation driver, address, handle, resolver, and unbounded list aren't
provided. An operational query is owned by the Location Runtime's paged
query with page size 1..1000 and encoded page at most 4 MiB.

If the cold Instance factory or initialize fails, that call completes
with a typed failure. The same call isn't hidden-retried internally, and
a public API for manipulating the failure state or recovery procedure
isn't provided.

`IZLinkSpotPublisherClient.Publish(...)` and
`IZLinkSpotOutbound.Publish(...)` are
[Logical Multicast](../../../../01-glossary.en.md#logical-multicast).
Both an external publisher's and a Spot callback's outbound only take
ChannelName and topic. A process-local
[ChannelName](../../../../01-glossary.en.md#channelname) index selects
the owner [MeshNode](../../../../01-glossary.en.md#meshnode), and the
caller doesn't additionally pass [MeshName](../../../../01-glossary.en.md#meshname).
Each remote target follows the MeshNode ROUTER's send rule, and matching
Spot queues on the same node share immutable message storage. The exact
configuration surface is owned by
[Topology Configuration §5](03-configuration-topology.en.md#5-publisher-and-runtime-option).
Per-target admission/failure results of remote transport and the local
Spot queue aren't returned or aggregated into monitoring. It doesn't wait
for remote Spot queue submission or remote/local handler execution or
completion.
