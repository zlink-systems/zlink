# .NET Actor Public Interface

A relocation including an Actor bound to a session keeps the same
`ObjectGeneration` and updates the bound-session current Actor location
snapshot to the target MeshName/NodeRid. Since relocation itself isn't a
physical/logical disconnect, it doesn't run the Actor disconnect
callback.

[.NET exact interface table of contents](README.en.md)

## 1. Actor

ActorId is a logical ID unique across the whole Location Store
transaction domain. Its UTF-8 encoded size is 1..255 bytes, it's compared
as a case-sensitive exact value, and it isn't normalized. A regular Actor
message only takes ActorId and resolves current authority. `ActorRef` is
the immutable location snapshot used to change an exact incarnation or
bind to a session.

```csharp
public readonly record struct ActorRef(
    string ActorId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId NodeRid);

public interface IZLinkActor
{
    IZLinkActorContext Context { get; }
    void Configure() { }
    ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkActorContext
{
    string ActorId { get; }
    ulong ObjectGeneration { get; }
    string MeshName { get; }
    string? SpotId { get; }
    IZLinkBoundSession BoundSession { get; }
    IZLinkActorJoinSpotCall JoinSpot(string spotId);
    IZLinkActorJoinSpotCall JoinSpot(
        string spotId,
        ZLinkMessage request);
    IZLinkActorJoinSpotCall JoinSpot<TRequest>(
        string spotId,
        TRequest request)
    {
        return JoinSpot(spotId, ZLinkMessage.From(request));
    }
    IZLinkActorJoinEntrySpotCall JoinEntrySpot();
    IZLinkActorJoinEntrySpotCall JoinEntrySpot(
        ZLinkMessage request);
    IZLinkActorJoinEntrySpotCall JoinEntrySpot<TRequest>(
        TRequest request)
    {
        return JoinEntrySpot(ZLinkMessage.From(request));
    }
}

public interface IZLinkActorHandlerRegistry
{
    void AddHandler<THandler>()
        where THandler : class;
    void AddHandler<THandler>(string packetName)
        where THandler : class;
    void AddActorPacket<THandler, TActor>()
        where THandler : class
        where TActor : IZLinkActor;
    void AddActorPacket<THandler, TActor>(string packetName)
        where THandler : class
        where TActor : IZLinkActor;
}

public interface IZLinkActorFactory
{
    ValueTask<IZLinkActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorFactory<TActor>
    : IZLinkActorFactory
    where TActor : class, IZLinkActor
{
    new ValueTask<TActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorRelocationAdapter<TActor>
    where TActor : class, IZLinkActor
{
    ValueTask<byte[]> CaptureAsync(
        TActor actor,
        CancellationToken cancellationToken);
    ValueTask RestoreAsync(
        TActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken);
}

public interface IZLinkActorClient
{
    IZLinkActorSendCall SendToActor<TMessage>(
        string actorId,
        TMessage message);
    IZLinkActorRequestCall RequestToActor<TRequest>(
        string actorId,
        TRequest request);
}

public interface IZLinkActorManager
{
    IZLinkActorCreateCall Create(
        string actorId,
        string actorType);
    IZLinkActorGetOrCreateCall GetOrCreate(
        string actorId,
        string actorType);
    ValueTask<ActorRef?> FindAsync(
        string actorId,
        CancellationToken cancellationToken = default);
    ValueTask<SpotRef?> FindSpotAsync(
        string actorId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> DestroyAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
}

public abstract record ZLinkActorCreateResult
{
    private protected ZLinkActorCreateResult() { }

    public sealed record Existing(ActorRef Actor)
        : ZLinkActorCreateResult;

    public sealed record Created(
        ActorRef Actor,
        ZLinkMessage? Reply)
        : ZLinkActorCreateResult;

    public sealed record Rejected(ZLinkMessage? Reply)
        : ZLinkActorCreateResult;
}

public interface IZLinkActorCreateCall
{
    IZLinkActorCreateCall InMesh(string meshName);
    IZLinkActorCreateCall Request(ZLinkMessage request);
    IZLinkActorCreateCall Request<TRequest>(TRequest request);
    IZLinkActorCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkActorCreateResult> Async(
        CancellationToken cancellationToken = default);
    ValueTask<ZLinkActorCreateResult> Yield(
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorGetOrCreateCall
{
    IZLinkActorGetOrCreateCall InMesh(string meshName);
    IZLinkActorGetOrCreateCall Request(ZLinkMessage request);
    IZLinkActorGetOrCreateCall Request<TRequest>(TRequest request);
    IZLinkActorGetOrCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkActorCreateResult> Async(
        CancellationToken cancellationToken = default);
    ValueTask<ZLinkActorCreateResult> Yield(
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorSendCall : IZLinkMetadataCall<IZLinkActorSendCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorRequestCall : IZLinkMetadataCall<IZLinkActorRequestCall>
{
    IZLinkActorRequestCall Timeout(TimeSpan timeout);
    ValueTask<TReply> Async<TReply>(
        CancellationToken cancellationToken = default);
    ValueTask<TReply> Yield<TReply>(
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorDeferredJoinCall
{
    void Defer();
}

public interface IZLinkActorJoinSpotCall : IZLinkActorDeferredJoinCall
{
    IZLinkActorJoinSpotCall Timeout(TimeSpan timeout);
}

public interface IZLinkActorJoinEntrySpotCall : IZLinkActorDeferredJoinCall
{
    IZLinkActorJoinEntrySpotCall Timeout(TimeSpan timeout);
}

public readonly record struct ZLinkActorJoinOperationId(ulong High, ulong Low);

public abstract record ZLinkActorJoinCompletion
{
    private protected ZLinkActorJoinCompletion() { }
    public sealed record Accepted(
        ZLinkActorJoinOperationId OperationId,
        ActorRef Actor,
        ZLinkMessage? Reply) : ZLinkActorJoinCompletion;
    public sealed record Rejected(
        ZLinkActorJoinOperationId OperationId,
        ZLinkMessage? Reply) : ZLinkActorJoinCompletion;
    public sealed record Failed(
        ZLinkActorJoinOperationId OperationId,
        ZLinkFrameworkErrorKind Kind) : ZLinkActorJoinCompletion;
}
```

The Actor packet handler is registered on the registry the Spot owns.
The handler's exact context and generic parameter are defined by the
[Spot Interface](05-spots.en.md). `SpotId == null` means the Entry Spot
stage, and a value present means it's a member of that user
[Spot](../../../../01-glossary.en.md#spot). A separate boolean
representing the same state isn't provided.

The Actor Join call only provides a resultless synchronous `Defer()`, and
doesn't provide `Async(...)`/`Yield(...)`. If a `SpotWide` User Spot's
member Actor yields an Actor/Spot/Channel request or worker call, the
Actor queue claim is kept and only the User Spot gate is returned. The
same Actor's next job doesn't start until the terminal continuation
re-acquires the gate and finishes the current job. On an Entry Spot and
`PerActor` User Spot Actor, it completes with `InvalidOperation` before
request/worker operation submit.

`Defer()` only registers an immutable Join intent and an inactive
barrier on the current handler, and doesn't start a target lookup or
Store I/O. If the handler finishes normally, the Join runs; if it fails,
the barrier is discarded. The target admission/relocation result is
delivered via the `OnJoinCompletedAsync(...)` callback with the same
128-bit operation ID. Even if the handler uses `Yield(...)`, the barrier
isn't activated until the last continuation finishes.

Operation ID is a completion idempotency ID, not a `RelocationId`,
reservation ID, or aggregate commit ID. Same-node and cross-node
completion retry are limited to the current source and target process
lifetime. After the process ends, a different runtime doesn't
automatically replay completion.

The overload with no request fixes an empty `ZLinkMessage`. The default
timeout is 5 seconds, and an explicit value is a finite `1..int.MaxValue`
ms rounded up to milliseconds. `Defer()` fixes a monotonic absolute
deadline.

Relocation policy is owned by the Actor factory registration.
`DisableRelocation` rejects, before capture, a move that requires
cross-node materialization. `RecreateOnRelocation` creates the same
logical identity again with the target
[factory](../../../../01-glossary.en.md#factory), without restoring
application state. `PreserveStateWith<TAdapter>()` stores the byte array
`IZLinkActorRelocationAdapter<TActor>` returns as an opaque application
payload and restores it to the target Actor instance. It doesn't take a
separate application state generic or a stable state contract ID, and
doesn't use a Framework message wrapper as the payload. The adapter isn't
given a relocation reference, accepted journal, relocation phase,
source/target owner, or Store CAS version.

For maintenance that materializes an Actor instance on a different node,
cross-node User Spot/[Entry Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
join, and every Actor participant of a whole User Spot relocation, the
same Actor factory policy is used. Only for `PreserveStateWith` are the
Actor adapter's `CaptureAsync(...)` and `RestoreAsync(...)` called. A
same-node join doesn't call the adapter and isn't rejected with
`DisableRelocation` either. A cross-node move on `DisableRelocation`
policy is rejected before capture, without an adapter.

The target finishes restore and accepted-journal validation/staging
before the [owner](../../../../01-glossary.en.md#owner) commit, without
running an application handler. After the owner commit and lifecycle
callback, the saved existing work is put on the actual Actor queue first,
and the relocation temporary queue's work is moved after that. Once
temporary queue registration is removed and dispatch is switched
atomically, the target opens as `Ready` and the relocation fence is
released. Source cleanup, the `Completed` record, and the bound-session
location update response don't block the target's message processing.
If the target process terminates after `Ready`, it's handled as ordinary
owner loss, and the previous relocation isn't automatically replayed. A
public phase API for manipulating this barrier isn't provided.

On a retry within the same source and target process, factory and
`RestoreAsync(...)` can be called more than once. `CaptureAsync(...)` can
also be called again before the authority commit. Both callbacks must be
retry-safe, producing the same result for the same logical relocation,
and must not depend on exactly-once execution of an external side
effect. A capture exception restores admission after a durable abort and
source normalization. `CaptureAsync(...)`'s result is at most 64 MiB; an
empty array is valid, and null is a contract violation. The framework
immediately copies the completed array. `RestoreAsync(...)`'s
`ReadOnlyMemory<byte>` is only valid until the callback completes. If a
restore exception occurs, the instance is discarded and the same
immutable payload is applied to a new instance. A different target isn't
automatically selected. If the framework cancels a callback due to the
operation deadline, it's classified as `DeadlineExceeded`. Only the
current exact owner and attempt fence can commit completion and open
admission, and a relocation ID isn't provided to the callback.

If connection-bound work already accepted before starting relocation
doesn't finish within the deadline, relocation is aborted and
`RelocateAsync(...)` completes with `Blocked/DeadlineExceeded`. A public
ACK or phase API to directly confirm or manipulate this isn't provided.

The order of lifecycle callbacks run during Entry Spot maintenance and a
regular join, sealed retry after a callback failure, and callback
omission for a whole User Spot aggregate move are determined by the
[Spot Interface](05-spots.en.md). The Actor relocation adapter doesn't
substitute for this lifecycle callback. There's no public phase API that
controls this order.

When creating a new distributed Actor, the framework reserves creation
authority and the target's pending capacity together so multiple targets
can't create the same Actor concurrently. This reservation is processed
in the following order.

1. The provider creates a `Creating` row on authority and secures the
   target's pending capacity together.
2. Only the target that secured the reservation first runs the factory
   and the Entry Spot's `OnCreateActorAsync(...)`.
3. If the callback approves, it commits initial Entry
   [membership](../../../../01-glossary.en.md#membership), `Ready`,
   active capacity, and the `Created` terminal result together.
4. If the callback rejects, it doesn't create Ready or active capacity,
   and publishes the `Rejected` terminal result while cleaning up the
   Creating row and pending capacity.
5. Node shutdown, timeout, or a callback exception is published as an
   `Aborted` failure, distinct from an application `Rejected`.
6. A target that loses the reservation race doesn't start a separate
   factory. It reads the existing reservation result the provider
   returned and joins the current creation attempt.

Resolve and remote messaging only use the `Ready` state. Entry Spot
initialization also completes before the host's `Serving` publication.
There's no application API that controls this barrier.

Actor factory options and relocation policy are registered together in
the `AddActorFactory<TActor,TFactory>(...)` configure callback of
[Topology Configuration](03-configuration-topology.en.md). The callback
must select exactly one policy.

The Create and GetOrCreate calls are single-use. Setting the same option
twice is `InvalidOperation`, and calling terminal `Async(...)` twice is
`InvalidOperation`. At the terminal call, one deadline is fixed that
applies across resolve, reservation, factory, and the Ready barrier. If
`InMesh(...)` is omitted and there's one object-role Mesh, that Mesh is
used; with 0, `NotConfigured`; with two or more, `InvalidOperation`. If
the specified Mesh doesn't exist, `NotFound`. The caller doesn't specify
a target RID, predicate, or callback.

`Create` returns `AlreadyExists` if a
[Ready](../../../../01-glossary.en.md#ready) incarnation of the same
ActorId exists, and `TypeMismatch` if stable type differs. `GetOrCreate`
returns a Ready Actor of the same type as `Existing`, and waits for the
authority change if it's a Creating attempt. A CAS loser doesn't run a
separate factory. A different operation receives `Existing` after Ready,
competes for a new reservation after cleanup, and doesn't share an
earlier application reply. Only a resend of the same source Node
RID/lifecycle generation/`OperationId` reads the correlation-free
`creation-operation-terminal-v1` envelope and re-encodes the reply with
the current correlation/reply route. The terminal is kept for 5 minutes
after the original deadline. If a Creating attempt doesn't finish within
the deadline, that caller gets `DeadlineExceeded`.

The creation request and semantic terminal envelope are each at most 1
MiB. The creation request records an immutable reference and hash before
reservation. The factory must be retry-safe for the same ID,
ObjectGeneration, and creation attempt.

`FindAsync(actorId)` only returns the current Ready `ActorRef`.
`FindSpotAsync(actorId)` only returns the `SpotRef` of the current User
Spot membership. A separate Actor directory and public handle/resolver
aren't provided. `DestroyAsync(actorRef)` only closes the exact
incarnation. If that incarnation doesn't exist, `false`; if the
generation differs, `InvalidOperation`; if in pre-commit seal,
`Unavailable` — it doesn't find the current ref and hidden-retry.

`ActorRef.ObjectGeneration` is `1..long.MaxValue`. `MeshName` and
`NodeRid` are the route [snapshot](../../../../01-glossary.en.md#snapshot)
at query time, and aren't included in logical identity. Even after
relocation, ActorId and
[ObjectGeneration](../../../../01-glossary.en.md#objectgeneration) are
kept, and a ref with the new location is issued. Regular messaging
doesn't fix the ref route.
