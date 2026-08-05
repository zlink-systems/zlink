# .NET Spot 공개 인터페이스

Session에 bind된 Actor를 포함한 Spot relocation은 같은 `ObjectGeneration`을 유지한다. Relocation 자체는
physical·logical disconnect가 아니므로 Actor disconnect callback을 실행하지 않는다.

[.NET exact interface 목차](README.ko.md)

## 1. Spot

SpotId는 UTF-8 encoded 크기 1..255 bytes의 `string`이며 Location Store transaction domain 전체에서
유일한 logical ID다. 비교는 case-sensitive exact match이고 normalization하지 않는다. 일반 message는 SpotId만 받고
current authority를 resolve한다. `SpotRef`는 exact incarnation을 닫을 때 사용하는 immutable location
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

`ZLinkSpotCloseReason`의 numeric 값은 `ExplicitClose=0`, `HostShutdown=1`, `RelocationOut=2`,
`IdleEvicted=3`이다. `IdleEvicted`는 Instance Spot 전용 이유이며 Entry Spot과 User Spot에는 전달하지
않는다. 유휴 판정 조건과 정리 뒤 재활성화 규칙은
[Spot 모델 §6.2](../../../../11-spot-model.ko.md#62-유휴-instance-spot-정리)가 소유한다.
`Deadline`은 closing operation의 absolute deadline이다. Framework는 callback invocation 전에는
`cleanupCancellationToken`을 취소하지 않고 [deadline](../../../../01-glossary.ko.md#deadline)이 끝날 때 취소한다. 이미 취소된 handler token을 재사용하지
않는다. Entry·User·Instance Spot만 callback을 받고 Actor별 closing callback은 제공하지 않는다. Host Shutdown은
Actor membership과 local instance가 유효한 상태에서 callback을 실행하고 completion 뒤 scope와 [authority](../../../../01-glossary.ko.md#authority)를
정리한다. Standalone Actor relocation은 Entry Spot을 닫지 않으므로 이 callback을 호출하지 않는다.

`IZLinkSpotRelocationAdapter<TSpot>`은 `PreserveStateWith<TAdapter>()`로 등록한다. Cross-node User·[Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) instance를
materialize할 때만 호출한다. Whole User Spot relocation에서는 [Spot](../../../../01-glossary.ko.md#spot) adapter가 Spot application payload를 처리하고,
각 member Actor의 payload는 Actor factory에 등록한 Actor adapter가 각각 처리한다. `RecreateOnRelocation()`은 adapter를 호출하지
않고 application state 없이 instance를 다시 만들며 `DisableRelocation()`은 capture 전에 cross-node 이동을 거부한다.

Spot adapter의 capture와 restore는 stable relocation attempt에서 at-least-once 호출될 수 있으므로 retry-safe해야
한다. `CaptureAsync(...)` 결과는 최대 64 MiB이며 빈 배열은 유효하고 null은 contract 위반이다. Framework는
완료된 배열을 즉시 복사하고 이후 application mutation을 관찰하지 않는다. `RestoreAsync(...)`의
`ReadOnlyMemory<byte>`는 callback 완료까지만 유효하므로 보관하려면 application이 복사해야 한다. Capture
exception은 durable abort와 source normalization 뒤 admission을 복원한다. Restore exception이 발생한 instance는
폐기하고, 새 attempt는 [factory](../../../../01-glossary.ko.md#factory)가 만든 새 instance에 같은 immutable payload를 적용한다. Framework가 operation
deadline 때문에 callback을 취소하면 `DeadlineExceeded`로 분류한다. Framework는 callback의 external side effect를
exactly-once로 실행한다고 보장하지 않는다.

Maintenance가 Actor를 다른 node의 Entry Spot에 복원하면 Actor adapter restore를 먼저 완료하고 Location authority와
Entry [membership](../../../../01-glossary.ko.md#membership)을 commit한다. 이 작업은
application membership 변경이 아니므로 target `OnJoinedActorAsync(...)`, source
`OnLeaveActorAsync(...)`와 relocation 전용 callback을 호출하지 않는다. Accepted
journal·queue·Actor timer를 복원하고 Location authority·membership을 commit한 뒤 Actor
message 처리를 시작한다. Bound Session 위치 갱신은 그 뒤
`sessionActorLocationUpdateReqMsg`와 `sessionActorLocationUpdateResMsg` send message로
수행하며 응답이 없어도 Actor 처리를 멈추지 않는다.
User Spot으로 향하는 일반 application join은 target의 `OnActorJoinAsync(...)`,
membership commit, target의 `OnJoinedActorAsync(...)` 순서를 유지한다. Entry Spot
복귀는 admission callback 없이 membership을 commit한 뒤 target Entry Spot의
`OnJoinedActorAsync(...)`를 호출한다.
`SpotWide` User Spot aggregate move와 `PerActor` User Spot의 Actor relocation도
application membership callback을 호출하지 않는다.

`PerActor` User Spot은 `RecreateOnRelocation` Spot policy만 허용하고 Spot relocation adapter를
등록하지 않는다. Spot field와 Spot-level application timer는 relocation 대상이
아니다. Target Spot authority를 먼저 전환한 뒤 Actor를 독립적으로 이전하며
`ToSpot`·Create·Join은 Spot authority, `ToActor`는 Actor별 current owner를 사용한다.
Target runtime-private shell은 같은 public SpotId와 ObjectGeneration을 사용하며 authority
전환 전에는 public lookup에 노출하지 않는다. Stale source route는 operation identity,
generation, deadline, correlation과 reply route를 보존해 relay한다. Actor queue seal부터
target admission까지 1초는 운영 목표이며 초과해도 relocation을 취소하거나 rollback하지 않는다.

`RelocationReady().Defer()`는 `SpotWide` factory가
`ApplicationSignaled` readiness mode를 선택한 Spot turn에서만 유효하다. `Defer()`는
현재 handler가 끝난 뒤 다음 application turn 앞에 relocation 경계를 등록한다.
Framework는 이동하지 않았거나 commit 전에 abort했으면 source에서 `Continued`,
이동했으면 target에서 `Relocated` completion을
`OnRelocationReadyCompletedAsync(...)`에 전달한다. 기본 구현은 no-op이다.
Callback 완료 전에는 보류한 message와 timer를 실행하지 않는다.

기본 `AnyTurnBoundary`, `PerActor`, Entry·Instance Spot, Spot turn 밖과 같은 turn의
중복 `Defer()`는 queue mutation 전에 `ZLinkFrameworkErrorKind.InvalidOperation`
오류로 끝난다. `Defer()` 뒤 같은 turn에서 다른 Framework operation을 시작해도
같은 오류다. Callback은 process recovery에서 다시 실행될 수 있으므로 override는
retry-safe해야 한다.

Spot과 Actor의 current location 조회는 manager가 global ID로 수행한다. Public resolver와 runtime handle은
제공하지 않는다. owner route와 generation 갱신 규칙은
[Spot 주소 메시징](../../../../16-spot-address-messaging.ko.md)을 따른다.

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

Framework timer는 owner Actor·Spot에 속한 logical registration이다. Cross-node relocation에서는 timer 이름,
handler type, period, `ZLinkTimerOptions`, scheduling cursor와 seal 시점의 pending tick을 relocation payload에
자동으로 포함한다. Application의 relocation adapter는 timer를 capture·restore하거나 target에서 다시 등록하지
않는다. Framework가 관리하는 timer resource는 payload에 포함하지 않고 target에서 logical registration으로
다시 만든다. Source는 queue를 seal한 뒤 새 tick을 dispatch하지 않으며 target은 Restore와 authority commit을
마치고 dispatch admission이 열린 뒤에만 복원한 pending tick과 다음 tick을 [owner](../../../../01-glossary.ko.md#owner) mailbox에 제출한다.

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

Entry·User·Instance SpotId는 UTF-8 encoded 크기 1..255 bytes의 global string key다. Stable type은 UTF-8 1..255 bytes이며 case-sensitive exact value로
비교하고 normalization하지 않는다. `SpotRef.ObjectGeneration`은 1..`long.MaxValue`다. MeshName과 NodeRid는
조회 시점의 route snapshot이며 identity key에 포함하지 않는다.

`IZLinkSpotOutbound`과 `IZLinkSpotClient`는 global SpotId를 받고 Spot 전용 call을 반환한다. 일반
call은 current Ready location만 resolve한다. SpotId가 없으면 send와 request 모두 `NotFound`로
완료한다. `InstanceSpot()`
또는 `InstanceSpot(instanceSpotType)`을 명시한 call만 Missing Instance Spot을 새로 만들고 초기화하여
사용할 수 있게 준비할 수 있다. 이 과정을 cold activation이라 한다. [Ready](../../../../01-glossary.ko.md#ready) Instance authority가 있는 call은
authority에 저장된 [stable type](../../../../01-glossary.ko.md#stable-type)을 사용하므로 caller가
type을 다시 제공할 필요가 없다. Instance marker를 사용했는데 existing authority가 User Spot이거나
명시한 stable type과 authority의 type이 다르면 `TypeMismatch`다.

[Cold activation](../../../../01-glossary.ko.md#cold-activation)에서 `InstanceSpot()`은 선택된 Mesh에 등록된
Instance Spot type이 하나일 때만 그
type을 사용한다. 등록 type이 여러 개면 `InstanceSpot(instanceSpotType)`으로 type을 명시해야
한다. 선택한 Mesh에 type이 없으면 `NotFound`, 여러 개인데 type을 생략하면
`InvalidOperation`으로 완료한다.
`InMesh`는 Missing Instance Spot을 처음 생성할 Mesh를 지정한다. Object Client 또는 Server role의
Mesh가 하나면 생략할 수 있다. 후보가 둘 이상인데 생략하면 `InvalidOperation`, 후보가 없으면
`NotConfigured`, 지정한 Mesh가 없으면 `NotFound`로 완료한다. 이 option은 Instance
marker가 있는 call에서만 유효하며 marker 없이 사용하면
`InvalidOperation`이다. Existing authority를 다른 Mesh나 owner로
이동시키지 않는다. Option을 사용해도 target node나 endpoint를 지정할 수는 없다. Request
`Timeout`은 resolve, cold activation, handler와 reply 전체의 deadline을 고정한다. One-way call은 선택한
MeshNode의 send deadline에 resolve, cold activation과 outbound admission을 포함한다. Metadata와 terminal
소유권은 각 call이 계속 유지하며, send는 `Async`, request는 `Async<TReply>` 또는
`Yield<TReply>`로 한 번만 제출한다. Instance marker와 각 option은 한 번만 설정할 수 있다.
`Yield<TReply>`는 `SpotWide` User Spot 또는 Instance Spot callback에서만 유효하다. Entry Spot,
`PerActor` User Spot, Entry Spot Actor, Node·Channel handler와 owner turn 밖의 client에서 호출하면
operation을 제출하거나 turn을 반납하지 않고 `InvalidOperation`으로 완료한다.

`IZLinkInstanceSpot`은 `IZLinkSpot`을 상속하지 않는 actor-free lifecycle interface다. Direct packet과 timer
handler만 등록할 수 있다. Actor handler나 Logical Multicast subscription을 등록하면 Framework는 `Ready`
commit 전에 activation을 거부한다.

Store-backed User Spot은 manager의 create operation으로 생성한다. Instance Spot은 명시적인
`InstanceSpot(...)` intent가 있는 최초 message로 cold activation을 시작한다. Existing `Ready` Spot이 있으면
같은 call이 current owner로 전달된다. Cold activation이 필요하면 Framework가 eligible target을 선택하고
초기화가 완료된 뒤 최초 message를 해당 Spot의 첫 job으로 정확히 한 번 처리한다. Caller가 target node,
activation driver나 내부 reservation을 직접 지정하거나 제어하는 API는 제공하지 않는다.

Source는 owner claim이나 수용 공간을 미리 확보하지 않는다. Target에 같은 Spot의 local
instance가 없을 때만 자신을 owner로 하는 `Creating` record와 수용 공간을 함께
확보한다. 이 작업에 성공한 target 하나만 factory와 initialization을 실행한다.

`CloseAsync(spotRef)`는 exact incarnation만 닫는다. 해당 incarnation이 없으면 `false`, generation이 다르면
`InvalidOperation`, pre-commit seal 중이면 `Unavailable`이다. User Spot에 Actor membership이 남아 있으면
`false`이며 Actor를 자동 leave·destroy하지 않는다. Framework는 current ref를 다시 찾아 다른 incarnation을
닫지 않는다.

`IZLinkSpotManager`는 User Spot의 명시적 create·get-or-create, resolve와 exact close만 제공한다. Manager에
Spot kind를 선택하는 인자나 Instance Spot create·get-or-create overload를 두지 않는다. Instance Spot의
생성 경로는 Spot 전용 message call의 명시적 `InstanceSpot(...)` opt-in 하나다. Instance Spot
구현이 자신의 lifecycle을 종료하는 `IZLinkInstanceSpotContext.CloseAsync()`는 남긴다.

User Spot Create와 GetOrCreate call은 single-use다. 같은 option을 두 번 설정하면 `InvalidOperation`, terminal
`Async(...)`를 두 번 호출하면 `InvalidOperation`이다. `InMesh(...)` 선택과 오류 및 전체
deadline 규칙은 Actor create와 같다. `Create`는 Framework가 새 global Spot ID를 발급한다. `GetOrCreate`는 같은
User Spot stable type의 Ready Spot을 `Existing`으로 반환한다. Creating이면 authority
변경을 기다리고, Ready가 되면 `Existing`, cleanup으로 Missing이 되면 새 reservation을
경쟁한다. CAS loser는 별도 factory를 실행하지 않는다. Kind나 type이 다르면 `TypeMismatch`, deadline 안에 terminal state가 되지 않으면
`DeadlineExceeded`다. Creation request는 최대 1 MiB이며 reservation 전에 immutable
reference와 hash로 보관한다.

Missing authority에 대한 첫 `InstanceSpot(...)` call이 kind, stable type과 initial Mesh를 creation intent에
기록한다. Ready authority에 대한 message는 SpotId만으로 current owner를 resolve한다. Owner loss 뒤
reactivation은 authority에 저장한 intent를 사용하며 marker가 없는 Missing call은 새 intent를 만들지
않는다. Public activation driver, address, handle, resolver와 unbounded list는 제공하지 않는다. 운영 조회는
Location runtime의 page size 1..1000, encoded page 4 MiB 이하인 paged query가 소유한다.

Cold Instance factory 또는 initialize가 실패하면 해당 call은 typed failure로 완료된다. 같은 call을 내부에서
숨겨 재시도하지 않으며, 실패 상태나 recovery 절차를 조작하는 public API는 제공하지 않는다.

`IZLinkSpotPublisherClient.Publish(...)`와 `IZLinkSpotOutbound.Publish(...)`는 [Logical Multicast](../../../../01-glossary.ko.md#logical-multicast)다.
외부 publisher와 Spot callback의 outbound는 모두 ChannelName과 topic만 받는다. Process-local [ChannelName](../../../../01-glossary.ko.md#channelname)
index가 owner [MeshNode](../../../../01-glossary.ko.md#meshnode)를 선택하며 caller는 [MeshName](../../../../01-glossary.ko.md#meshname)을 추가로 넘기지 않는다.
각 remote target은 MeshNode ROUTER의 송신 규칙을 따르며, 같은 node의 일치하는 Spot queue는 immutable
message storage를 공유한다. 정확한 설정 표면은
[Topology configuration §5](03-configuration-topology.ko.md#5-publisher와-runtime-option)가 소유한다.
Remote transport와 local Spot queue의 target별 수락·실패 결과는 반환하거나 monitoring에 집계하지
않는다. Remote Spot queue 제출과 remote·local handler 실행 또는 완료는 기다리지 않는다.
