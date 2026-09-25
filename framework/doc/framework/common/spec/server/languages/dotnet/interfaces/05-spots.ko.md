# .NET Spot 공개 인터페이스

Session에 bind된 Actor를 포함한 Spot relocation은 같은 `ObjectGeneration`을 유지한다. Relocation 자체는
physical·logical disconnect가 아니므로 Actor disconnect callback을 실행하지 않는다.

[.NET 언어별 interface 목차](README.ko.md)

## 1. Spot

SpotId는 UTF-8 encoded 크기 1..255 bytes의 `string`이며 Location Store transaction domain 전체에서
유일한 logical ID다. 비교는 case-sensitive 비교이고 normalization하지 않는다. 일반 message는 SpotId만 받고
current authority를 resolve한다. `SpotRef`는 지정한 incarnation을 닫을 때 사용하는 immutable location
snapshot이며 runtime resource나 local Spot instance를 소유하지 않는다.

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
 // 기본 lifecycle은 생성 요청을 수락한다.
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
 // Application이 round 경계 후속 처리가 필요할 때만 override한다.
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
 // 연결 단절 처리가 필요하지 않은 Spot은 이 callback을 구현하지 않아도 된다.
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
 // 기본 lifecycle은 Actor 생성을 승인한다.
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

Framework는 Spot packet·request·subscription·timer handler를 해당 Spot의 activation
DI scope에서 한 번 만들고 Spot activation 동안 재사용한다. Actor send·request
handler는 해당 Actor의 별도 activation DI scope에서 한 번 만들고 Actor activation 동안
재사용한다. Entry Spot과 `PerActor` User Spot의 서로 다른 Actor는 handler instance와
scoped dependency를 공유하지 않는다.

Handler type 자체의 DI 등록 lifetime은 이 규칙을 바꾸지 않는다. Framework는
handler instance를 소유하고 생성자 dependency만 해당 activation scope에서 resolve한다.
별도의 handler lifetime option은 제공하지 않는다. Spot·Actor relocation과
cross-node Join에서는 source handler와 scope를 정리하고 target activation에서 다시
만든다. 복구해야 하는 상태는 handler field가 아니라 `TSpot` 또는 `TActor`가 소유한다.

`ZLinkSpotCloseReason`의 값은 `ExplicitClose=0`, `HostShutdown=1`, `RelocationOut=2`, `IdleEvicted=3`이다. Close와 cleanup은 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)이 정한다. Framework는 `OnClosingAsync`를 호출하기 전에 `cleanupCancellationToken`을 취소하지 않고, closing deadline이 끝나면 취소하며, 이미 취소된 handler token을 다시 쓰지 않는다.

`IZLinkSpotRelocationAdapter<TSpot>`은 `PreserveStateWith<TAdapter>()`로 등록한다. Adapter 호출 조건은 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)이 정한다.

.NET Spot adapter data는 `byte[]`와 `ReadOnlyMemory<byte>`를 사용한다. 재시도, payload 전송과 실패 처리는 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

Entry Spot maintenance와 Join의 callback·membership 순서는 [Spot–Actor membership](../../../03-spot-actor/05-spot-actor-membership.ko.md)이 정한다.

PerActor 이동과 보존된 relay는 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)과 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

.NET은 `RelocationReady().Defer()`와 `OnRelocationReadyCompletedAsync(...)`를 제공한다. 유효 문맥과 완료 경계는 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)이 정한다.

제출 전 오류는 [Execution gate](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)가 정한다. .NET은 `ZLinkFrameworkErrorKind.InvalidOperation`으로 표현한다.

Spot과 Actor의 current location 조회는 manager가 global ID로 수행한다. Public resolver와 runtime handle은
제공하지 않는다. owner route와 generation 갱신 규칙은
[Spot 주소 메시징](../../../03-spot-actor/06-spot-address-messaging.ko.md)을 따른다.

Spot handler signatures는 다음과 같다.

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

Timer option을 생략하면 `OverrunPolicy`는 `SkipLateTicks`, `MaxCatchUpTicks`는 `1`이다.
`MaxCatchUpTicks`는 `OverrunPolicy == CatchUpBounded`일 때만 사용하고 `1..Int32.MaxValue` 범위인지
검증한다. 다른 policy에서는 이 값을 사용하지 않으며 이 범위로 validation하지 않는다. 이 설명은 기존
`ZLinkTimerOptions` public surface를 바꾸지 않는다.

Relocation 중 logical timer 등록과 pending tick 처리는 [Spot timer](../../../03-spot-actor/10-spot-timer.ko.md)와 [Host relocation](../../../05-location-relocation/05-host-relocation-flow.ko.md)이 정한다.

Spot 외부 client는 다음 시그니처를 사용한다.

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

Spot ID와 route snapshot은 [Spot 주소 메시징](../../../03-spot-actor/06-spot-address-messaging.ko.md)이 정한다. .NET의 `SpotRef.ObjectGeneration`은 `long`이다.

`IZLinkSpotOutbound`과 `IZLinkSpotClient`는 global SpotId를 받고
`IZLinkSpotSendCall` 또는 `IZLinkSpotRequestCall`을 반환한다. Marker overload는
`InstanceSpot()`과 `InstanceSpot(string instanceSpotType)`이며, Mesh 입력은
`InMesh(string meshName)`이다. Send의 `Async(...)`는 `ValueTask`, request의
`Async<TReply>(...)`·`Yield<TReply>(...)`는 `ValueTask<TReply>`를 반환한다.

Cold activation의 type·Mesh 선택, 생성 순서와 최초 message 보존은
[Spot address messaging §4](../../../03-spot-actor/06-spot-address-messaging.ko.md#4-cold-activation--message로-instance-spot을-처음-만드는-방법)가 소유한다. 완료 경계는
[Spot address messaging §5](../../../03-spot-actor/06-spot-address-messaging.ko.md#5-existing-owner를-향한-direct-call과-완료-경계)를 따른다.

Instance marker와 `InMesh`의 유효성은 [Spot 주소 메시징](../../../03-spot-actor/06-spot-address-messaging.ko.md)이 정한다. .NET은 잘못된 사용을 `InvalidOperation`으로 표현한다.

`IZLinkInstanceSpot`은 별도의 .NET interface다. Handler와 activation 제약은 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)이 정한다.

Spot close 결과는 [Object lifecycle](../../../03-spot-actor/09-object-lifecycle.ko.md)이 정한다. .NET은 `false`, `InvalidOperation`, `Unavailable`로 표현한다.

`IZLinkSpotManager`는 User Spot의 명시적 create·get-or-create, resolve와 close만 제공한다. Manager에
Spot kind를 선택하는 인자나 Instance Spot create·get-or-create overload를 두지 않는다. Instance Spot의
생성 경로는 Spot 전용 message call의 명시적 `InstanceSpot(...)` opt-in 하나다. Instance Spot
구현이 자신의 lifecycle 종료를 요청하는 `IZLinkInstanceSpotContext.Close()`는 남긴다([Spot 주소 메시징 §7](../../../03-spot-actor/06-spot-address-messaging.ko.md#7-close와-generation-경계)).

Create·GetOrCreate 결과는 [Spot 주소 메시징](../../../03-spot-actor/06-spot-address-messaging.ko.md)과 [Object lifecycle](../../../03-spot-actor/09-object-lifecycle.ko.md)이 정한다. .NET은 `Async(...)`를 제공하고 오류를 `InvalidOperation`, `TypeMismatch`, `DeadlineExceeded`로 표현한다.

Stored creation intent의 재개는 [Object lifecycle §3](../../../03-spot-actor/09-object-lifecycle.ko.md)이, 운영 query 한도는 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

Cold activation 실패는 [Spot 주소 메시징](../../../03-spot-actor/06-spot-address-messaging.ko.md)이 정한다.

`IZLinkSpotPublisherClient.Publish(...)`와 `IZLinkSpotOutbound.Publish(...)`는 .NET Logical Multicast API다. 라우팅과 terminal 결과는 [Submit과 completion](../../../01-execution/01-submit-and-completion.ko.md)이 정한다.
