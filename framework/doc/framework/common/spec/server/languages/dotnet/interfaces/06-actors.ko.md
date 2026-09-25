# .NET Actor 공개 인터페이스

Session에 bind된 Actor를 포함한 relocation은 같은 `ObjectGeneration`을 유지하고 bound-session current
Actor location snapshot을 target MeshName·NodeRid로 갱신한다. Relocation 자체는 physical·logical
disconnect가 아니므로 Actor disconnect callback을 실행하지 않는다.

[.NET 언어별 interface 목차](README.ko.md)

## 1. Actor

ActorId는 Location Store transaction domain 전체에서 유일한 logical ID다. UTF-8 encoded 크기는
1..255 bytes이고 case-sensitive 값 비교로 비교하며 normalization하지 않는다. 일반 Actor message는
ActorId만 받고 current authority를 resolve한다. `ActorRef`는 지정한 incarnation을 변경하거나 session에
bind할 때 사용하는 immutable location snapshot이다.

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

Actor packet handler는 Spot이 소유한 registry에 등록한다. Handler의 정확한 context와 generic parameter는
[Spot interface](05-spots.ko.md)가 정의한다. `SpotId == null`은 Entry Spot 단계이고 값이 있으면 해당 user
[Spot](../../../00-foundation/02-glossary.ko.md#spot)에 참여한 상태다. 같은 상태를 나타내는 별도 boolean은 제공하지 않는다.

Actor Join call은 결과 없는 동기 `Defer()`만 제공하고 `Async(...)`·`Yield(...)`를 제공하지 않는다.
Actor request의 `Yield` 중 gate와 claim 처리는
[Handler turn과 execution gate](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)가 정한다.

`Defer()` 등록과 barrier 활성화는
[Handler turn과 execution gate](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)가 정한다.
.NET은 결과를 `OnJoinCompletedAsync(...)` callback으로 전달한다.

[Actor Join completion](../../../03-spot-actor/05-spot-actor-membership.ko.md#actor-join-completion)이 Operation ID의 목적과 수명을 정한다.

Request 없는 overload는 empty `ZLinkMessage`를 고정한다. Timeout 기본값은 5초이고
명시 값은 millisecond 올림 기준 유한한 `1..int.MaxValue` ms다. `Defer()`에서
monotonic absolute deadline을 고정한다.

`PreserveStateWith<TAdapter>()`는 `IZLinkActorRelocationAdapter<TActor>`를 사용한다. Actor relocation policy와 payload 전송은 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

Actor 이동별 adapter 호출 여부는 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

Queue cutover와 Ready admission은 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

.NET adapter는 `CaptureAsync(...)`와 `RestoreAsync(...)`에 `byte[]`·`ReadOnlyMemory<byte>`를 사용한다. 재시도, 실패와 completion 권한은 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)과 [Failure and failover policy](../../../05-location-relocation/06-failure-failover-policy.ko.md)가 정한다.

Relocation 전 drain과 deadline 결과는 [Host relocation](../../../05-location-relocation/05-host-relocation-flow.ko.md)이 정한다.

Entry Spot maintenance와 일반 join에서 실행하는 lifecycle callback의 순서, callback 실패 뒤 sealed retry와 whole
User Spot aggregate move의 callback 생략은 [Spot interface](05-spots.ko.md)가 정한다. Actor relocation adapter는 이
lifecycle callback을 대신하지 않는다. 이 순서를 제어하는 public phase API는 없다.

Actor 생성 예약과 Ready barrier는 [Actor 모델](../../../03-spot-actor/04-actor-model.ko.md)이 정한다.

Actor factory option과 relocation policy는
[Topology configuration](03-configuration-topology.ko.md)의 `AddActorFactory<TActor,TFactory>(...)` configure
callback에서 함께 등록한다. Callback에서 policy를 정확히 하나 선택해야 한다.

`Create`와 `GetOrCreate` call의 single-use, 중복 option과 terminal 재호출 오류는
[Actor 모델 §6.2](../../../03-spot-actor/04-actor-model.ko.md#62-create와-getorcreate-입력)가 소유한다.

Create deadline과 Mesh 선택은 [Actor 모델](../../../03-spot-actor/04-actor-model.ko.md)이 정한다. .NET은 선택 실패를 `NotConfigured`, `InvalidOperation`, `NotFound`로 표현한다.

Create·GetOrCreate 결과와 terminal 재전송은 [Actor 모델](../../../03-spot-actor/04-actor-model.ko.md), [Object lifecycle](../../../03-spot-actor/09-object-lifecycle.ko.md)과 [Framework API](../../../00-foundation/06-framework-api.ko.md)가 정한다. .NET 결과 이름은 `AlreadyExists`, `TypeMismatch`, `Existing`, `DeadlineExceeded`다.

Creation request와 semantic terminal envelope는 각각 최대 1 MiB다. Creation request는 reservation 전에 immutable reference와 hash를
기록한다. Factory는 같은 ID, ObjectGeneration과 creation attempt에 대해
retry-safe해야 한다.

`FindAsync(actorId)`는 current Ready `ActorRef`만 반환한다. `FindSpotAsync(actorId)`는 current User Spot
membership의 `SpotRef`만 반환한다. 별도 Actor directory와 public handle·resolver는 제공하지 않는다.
`DestroyAsync(actorRef)`는 지정한 incarnation만 종료한다. 해당 incarnation이 없으면 `false`, generation이
다르면 `InvalidOperation`, pre-commit seal 중이면 `Unavailable`이며 current ref를 찾아 hidden retry하지
않는다.

`ActorRef.ObjectGeneration`은 1..`long.MaxValue`다. `MeshName`과 `NodeRid`는 조회 시점의 route [snapshot](../../../00-foundation/02-glossary.ko.md#snapshot)이며
logical identity에는 포함되지 않는다. Relocation 뒤에도 ActorId와 [ObjectGeneration](../../../00-foundation/02-glossary.ko.md#objectgeneration)은 유지되고 새 location을
가진 ref가 발급된다. 일반 messaging은 ref route를 고정하지 않는다.
