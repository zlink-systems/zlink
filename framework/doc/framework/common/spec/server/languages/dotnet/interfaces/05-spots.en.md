# .NET Spot Public Interface

A Spot relocation, including an Actor bound to a session, keeps the same
`ObjectGeneration`. Since relocation itself isn't a physical/logical
disconnect, it doesn't run the Actor disconnect callback.

[.NET per-language interface table of contents](README.en.md)

## 1. Spot

SpotId is a `string` with UTF-8 encoded size 1..255 bytes, and is a
logical ID unique across the whole Location Store transaction domain.
Comparison is case-sensitive comparison, with no normalization. A
regular message only takes SpotId and resolves current authority.
`SpotRef` is the immutable location snapshot used when closing an
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

 void Close();
}

public interface IZLinkInstanceSpotContext : IZLinkSpotCommonContext
{
 IZLinkInstanceSpotHandlerRegistry Handlers { get; }

 void Close();
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

`ZLinkSpotCloseReason` maps to `ExplicitClose=0`, `HostShutdown=1`, `RelocationOut=2`, and `IdleEvicted=3`; [Spot model](../../../03-spot-actor/01-spot-model.en.md) defines close and cleanup behavior. The framework does not cancel `cleanupCancellationToken` before invoking `OnClosingAsync`; it cancels the token when the closing deadline ends and does not reuse an already canceled handler token.

`IZLinkSpotRelocationAdapter<TSpot>` is registered with `PreserveStateWith<TAdapter>()`; [Spot model](../../../03-spot-actor/01-spot-model.en.md) defines when the adapter runs.

.NET uses `byte[]` and `ReadOnlyMemory<byte>` for Spot adapter data; [Location runtime](../../../05-location-relocation/01-location-runtime.en.md) defines retry, payload transfer, and failure handling.

[Spot–Actor membership](../../../03-spot-actor/05-spot-actor-membership.en.md) defines callback and membership order for Entry Spot maintenance and joins.

[Spot model](../../../03-spot-actor/01-spot-model.en.md) and [Location runtime](../../../05-location-relocation/01-location-runtime.en.md) define PerActor moves and preserved relay.

`.NET` exposes `RelocationReady().Defer()` and `OnRelocationReadyCompletedAsync(...)`; [Spot model](../../../03-spot-actor/01-spot-model.en.md) defines their eligibility and completion boundary.

[Execution gate](../../../01-execution/02-handler-turn-and-execution-gate.en.md) defines the pre-mutation error; .NET maps it to `ZLinkFrameworkErrorKind.InvalidOperation`.

The current-location query for Spot and Actor is performed by the
manager using the global ID. A public resolver and runtime handle aren't
provided. The owner route and generation update rule follows
[Spot Address Messaging](../../../03-spot-actor/06-spot-address-messaging.en.md).

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

When timer options are omitted, `OverrunPolicy` defaults to `SkipLateTicks`
and `MaxCatchUpTicks` defaults to `1`. `MaxCatchUpTicks` is used and validated
in `1..Int32.MaxValue` only when `OverrunPolicy == CatchUpBounded`. Other
policies do not use or validate this value against that range. This prose does
not change the existing `ZLinkTimerOptions` public surface.

[Spot timer](../../../03-spot-actor/10-spot-timer.en.md) and [Host relocation](../../../05-location-relocation/05-host-relocation-flow.en.md) define logical timer registration and pending ticks during relocation.

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

[Spot address messaging](../../../03-spot-actor/06-spot-address-messaging.en.md) defines Spot IDs and route snapshots; .NET projects `SpotRef.ObjectGeneration` as `long`.

`IZLinkSpotOutbound` and `IZLinkSpotClient` take the global SpotId and return
`IZLinkSpotSendCall` or `IZLinkSpotRequestCall`. Marker overloads are `InstanceSpot()`
and `InstanceSpot(string instanceSpotType)`; the Mesh input is `InMesh(string meshName)`.
Send `Async(...)` returns `ValueTask`; request `Async<TReply>(...)` and
`Yield<TReply>(...)` return `ValueTask<TReply>`.

[Spot address messaging §4](../../../03-spot-actor/06-spot-address-messaging.en.md#4-cold-activation--how-to-create-an-instance-spot-for-the-first-time-via-a-message) owns type/Mesh selection, the creation sequence, and first-message
preservation for cold activation. Completion boundaries follow [Spot address messaging §5](../../../03-spot-actor/06-spot-address-messaging.en.md#5-direct-call-to-an-existing-owner-and-the-completion-boundary).

[Spot address messaging](../../../03-spot-actor/06-spot-address-messaging.en.md) defines Instance marker and `InMesh` validity; .NET maps invalid use to `InvalidOperation`.

`IZLinkInstanceSpot` is a separate .NET interface; [Spot model](../../../03-spot-actor/01-spot-model.en.md) defines its handler and activation constraints.

[Object lifecycle](../../../03-spot-actor/09-object-lifecycle.en.md) defines Spot close outcomes; .NET maps them to `false`, `InvalidOperation`, and `Unavailable`.

`IZLinkSpotManager` only provides User Spot's explicit create/
get-or-create, resolve, and close. The manager doesn't have an
argument to select Spot kind or an Instance Spot create/get-or-create
overload. Instance Spot's creation path is the one explicit
`InstanceSpot(...)` opt-in on the Spot-dedicated message call. It leaves
`IZLinkInstanceSpotContext.Close()` for an Instance Spot implementation to
request the end of its own lifecycle
([Spot address messaging §7](../../../03-spot-actor/06-spot-address-messaging.en.md#7-close-and-the-generation-boundary)).

[Spot address messaging](../../../03-spot-actor/06-spot-address-messaging.en.md) and [Object lifecycle](../../../03-spot-actor/09-object-lifecycle.en.md) define Create/GetOrCreate results; .NET exposes `Async(...)` and maps errors to `InvalidOperation`, `TypeMismatch`, and `DeadlineExceeded`.

[Object lifecycle §3](../../../03-spot-actor/09-object-lifecycle.en.md) defines stored creation intent resumption; [Location runtime](../../../05-location-relocation/01-location-runtime.en.md) defines operational query limits.

[Spot address messaging](../../../03-spot-actor/06-spot-address-messaging.en.md) defines cold activation failure.

`IZLinkSpotPublisherClient.Publish(...)` and `IZLinkSpotOutbound.Publish(...)` are the .NET Logical Multicast APIs; [Submit and completion](../../../01-execution/01-submit-and-completion.en.md) defines routing and terminal results.
