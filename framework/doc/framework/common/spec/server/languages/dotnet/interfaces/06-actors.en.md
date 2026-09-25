# .NET Actor Public Interface

A relocation including an Actor bound to a session keeps the same
`ObjectGeneration` and updates the bound-session current Actor location
snapshot to the target MeshName/NodeRid. Since relocation itself isn't a
physical/logical disconnect, it doesn't run the Actor disconnect
callback.

[.NET per-language interface table of contents](README.en.md)

## 1. Actor

ActorId is a logical ID unique across the whole Location Store
transaction domain. Its UTF-8 encoded size is 1..255 bytes, it's compared
as a case-sensitive value comparison, and it isn't normalized. A regular Actor
message only takes ActorId and resolves current authority. `ActorRef` is
the immutable location snapshot used to change an specified incarnation or
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
The handler's context and generic parameter are defined by the
[Spot Interface](05-spots.en.md). `SpotId == null` means the Entry Spot
stage, and a value present means it's a member of that user
[Spot](../../../00-foundation/02-glossary.en.md#spot). A separate boolean
representing the same state isn't provided.

The Actor Join call provides only a resultless synchronous `Defer()`, without
`Async(...)` or `Yield(...)`. [Handler turn and execution gate](../../../01-execution/02-handler-turn-and-execution-gate.en.md)
defines gate and claim handling when an Actor request yields.

[Handler turn and execution gate](../../../01-execution/02-handler-turn-and-execution-gate.en.md)
defines `Defer()` registration and barrier activation.
.NET delivers the result through `OnJoinCompletedAsync(...)`.

[Actor Join completion](../../../03-spot-actor/05-spot-actor-membership.en.md#actor-join-completion) owns the Operation ID purpose and lifetime.

The overload with no request fixes an empty `ZLinkMessage`. The default
timeout is 5 seconds, and an explicit value is a finite `1..int.MaxValue`
ms rounded up to milliseconds. `Defer()` fixes a monotonic absolute
deadline.

`PreserveStateWith<TAdapter>()` uses `IZLinkActorRelocationAdapter<TActor>`; [Location runtime](../../../05-location-relocation/01-location-runtime.en.md) defines Actor relocation policy and payload transfer.

[Location runtime](../../../05-location-relocation/01-location-runtime.en.md) defines which Actor moves invoke the adapter.

[Location runtime](../../../05-location-relocation/01-location-runtime.en.md) defines queue cutover and Ready admission.

`CaptureAsync(...)` and `RestoreAsync(...)` use `ReadOnlyMemory<byte>` and `byte[]` in .NET; [Location runtime](../../../05-location-relocation/01-location-runtime.en.md) and [Failure and failover policy](../../../05-location-relocation/06-failure-failover-policy.en.md) define retry, failure, and completion authority.

[Host relocation](../../../05-location-relocation/05-host-relocation-flow.en.md) defines the pre-relocation drain and deadline result.

The order of lifecycle callbacks run during Entry Spot maintenance and a
regular join, sealed retry after a callback failure, and callback
omission for a whole User Spot aggregate move are determined by the
[Spot Interface](05-spots.en.md). The Actor relocation adapter doesn't
substitute for this lifecycle callback. There's no public phase API that
controls this order.

[Actor model](../../../03-spot-actor/04-actor-model.en.md) defines creation reservation and the Ready barrier.

Actor factory options and relocation policy are registered together in
the `AddActorFactory<TActor,TFactory>(...)` configure callback of
[Topology Configuration](03-configuration-topology.en.md). The callback
must select exactly one policy.

The [Actor Model §6.2](../../../03-spot-actor/04-actor-model.en.md#62-create-and-getorcreate-input) owns the single-use rules, duplicate-option handling, and terminal
re-invocation errors of `Create` and `GetOrCreate` calls.

[Actor model](../../../03-spot-actor/04-actor-model.en.md) defines the create deadline and Mesh selection. .NET maps the selection failures to `NotConfigured`, `InvalidOperation`, and `NotFound`.

[Actor model](../../../03-spot-actor/04-actor-model.en.md), [Object lifecycle](../../../03-spot-actor/09-object-lifecycle.en.md), and [Framework API](../../../00-foundation/06-framework-api.en.md) define Create/GetOrCreate outcomes and terminal replay. .NET projects `AlreadyExists`, `TypeMismatch`, `Existing`, and `DeadlineExceeded`.

The creation request and semantic terminal envelope are each at most 1
MiB. The creation request records an immutable reference and hash before
reservation. The factory must be retry-safe for the same ID,
ObjectGeneration, and creation attempt.

`FindAsync(actorId)` only returns the current Ready `ActorRef`.
`FindSpotAsync(actorId)` only returns the `SpotRef` of the current User
Spot membership. A separate Actor directory and public handle/resolver
aren't provided. `DestroyAsync(actorRef)` only closes the
incarnation. If that incarnation doesn't exist, `false`; if the
generation differs, `InvalidOperation`; if in pre-commit seal,
`Unavailable` — it doesn't find the current ref and hidden-retry.

`ActorRef.ObjectGeneration` is `1..long.MaxValue`. `MeshName` and
`NodeRid` are the route [snapshot](../../../00-foundation/02-glossary.en.md#snapshot)
at query time, and aren't included in logical identity. Even after
relocation, ActorId and
[ObjectGeneration](../../../00-foundation/02-glossary.en.md#objectgeneration) are
kept, and a ref with the new location is issued. Regular messaging
doesn't fix the ref route.
