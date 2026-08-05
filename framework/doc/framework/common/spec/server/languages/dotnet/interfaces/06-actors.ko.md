# .NET Actor 공개 인터페이스

Session에 bind된 Actor를 포함한 relocation은 같은 `ObjectGeneration`을 유지하고 bound-session current
Actor location snapshot을 target MeshName·NodeRid로 갱신한다. Relocation 자체는 physical·logical
disconnect가 아니므로 Actor disconnect callback을 실행하지 않는다.

[.NET exact interface 목차](README.ko.md)

## 1. Actor

ActorId는 Location Store transaction domain 전체에서 유일한 logical ID다. UTF-8 encoded 크기는
1..255 bytes이고 case-sensitive exact value로 비교하며 normalization하지 않는다. 일반 Actor message는
ActorId만 받고 current authority를 resolve한다. `ActorRef`는 exact incarnation을 변경하거나 session에
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
[Spot](../../../../01-glossary.ko.md#spot)에 참여한 상태다. 같은 상태를 나타내는 별도 boolean은 제공하지 않는다.

Actor Join call은 결과 없는 동기 `Defer()`만 제공하고 `Async(...)`·`Yield(...)`를
제공하지 않는다. `SpotWide` User Spot의 member Actor가 Actor·Spot·Channel request
또는 worker call을 `Yield(...)`하면 Actor queue claim은 유지하고 User Spot gate만
반환한다. 같은 Actor의 다음 job은 terminal continuation이 gate를 다시 얻어 현재
job을 완료할 때까지 시작하지 않는다. Entry Spot과 `PerActor` User Spot Actor에서는
request·worker operation submit 전에 `InvalidOperation`으로 완료한다.

`Defer()`는 현재 handler에 immutable Join intent와 비활성 barrier만 등록하며 target
조회나 Store I/O를 시작하지 않는다. Handler가 정상적으로 끝나면 Join을 실행하고
실패하면 barrier를 폐기한다. Target admission·relocation 결과는 같은 128-bit
operation ID의 `OnJoinCompletedAsync(...)` callback으로 전달한다. Handler가
`Yield(...)`를 사용해도 barrier는 마지막 continuation이 끝나기 전에는 활성화하지
않는다.

Operation ID는 completion idempotency ID이며 `RelocationId`, reservation ID와
aggregate commit ID가 아니다. Same-node와 cross-node completion retry는 current
source와 target process lifetime으로 제한한다. Process 종료 뒤 다른 runtime이
completion을 자동 replay하지 않는다.

Request 없는 overload는 empty `ZLinkMessage`를 고정한다. Timeout 기본값은 5초이고
명시 값은 millisecond 올림 기준 유한한 `1..int.MaxValue` ms다. `Defer()`에서
monotonic absolute deadline을 고정한다.

Relocation policy는 Actor factory registration이 소유한다. `DisableRelocation`은 cross-node materialization이 필요한
이동을 capture 전에 거부한다. `RecreateOnRelocation`은 target [factory](../../../../01-glossary.ko.md#factory)로 같은 logical identity를 다시 만들고 application
state를 복구하지 않는다. `PreserveStateWith<TAdapter>()`는 `IZLinkActorRelocationAdapter<TActor>`가 반환한 byte 배열을
opaque application payload로 저장하고 target Actor instance에 복원한다. 별도 application state generic과 stable
state contract ID를 받지 않으며 Framework message wrapper를 payload로 사용하지 않는다. Adapter는 relocation
reference, accepted journal, relocation phase, source·target owner와 Store CAS version을 받지 않는다.

다른 node에서 Actor instance를 materialize하는 maintenance, cross-node User Spot·[Entry Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) join과 whole User
Spot relocation의 모든 Actor participant는 같은 Actor factory policy를 사용한다. `PreserveStateWith`일 때만 Actor adapter의
`CaptureAsync(...)`와 `RestoreAsync(...)`를 호출한다. Same-node join은 adapter를 호출하지 않으며 `DisableRelocation`으로
거부하지도 않는다. `DisableRelocation` policy의 cross-node 이동은 adapter 없이 capture 전에 거부한다.

Target은 [owner](../../../../01-glossary.ko.md#owner) commit 전에 restore와 accepted journal validation·staging을 완료하며 application handler를
실행하지 않는다. Owner commit과 lifecycle callback 뒤 저장된 기존 작업을 실제 Actor queue에 먼저 넣고
relocation temporary queue의 작업을 그 뒤에 옮긴다. Temporary queue 등록을 제거하고 dispatch를 atomic하게
전환한 뒤 target을 `Ready`로 열고 relocation fence를 해제한다. Source cleanup, `Completed` 기록과
bound-session 위치 갱신 응답은 target message 처리를 막지 않는다. `Ready` 뒤 target process가 종료되면
ordinary owner loss로 처리하며 이전 relocation을 자동 replay하지 않는다. 이 barrier를 조작하는 public
phase API는 제공하지 않는다.

같은 source와 target process 안의 재시도에서 factory와 `RestoreAsync(...)`를 두 번 이상
호출할 수 있다. `CaptureAsync(...)`도 authority commit 전에 다시 호출될 수 있다. 두 callback은 같은 logical relocation에 대해 같은
결과를 내도록 retry-safe해야 하며 외부 side effect의 exactly-once 실행에 의존하면 안 된다. Capture exception은
durable abort와 source normalization 뒤 admission을 복원한다. `CaptureAsync(...)` 결과는 최대 64 MiB이며 빈
배열은 유효하고 null은 contract 위반이다. Framework는 완료된 배열을 즉시 복사한다. `RestoreAsync(...)`의
`ReadOnlyMemory<byte>`는 callback 완료까지만 유효하다. Restore exception이 발생한 instance는 폐기하고 새
instance에 같은 immutable payload를 적용한다. 다른 target을 자동 선택하지 않는다. Framework가 operation deadline 때문에
callback을 취소하면 `DeadlineExceeded`로 분류한다. Current exact owner와 attempt fence만 completion을 commit하고
admission을 열 수 있으며 callback에는 relocation ID를 제공하지 않는다.

Relocation을 시작하기 전에 이미 수락한 connection-bound work가 deadline 안에 끝나지 않으면 relocation을
중단하고 `RelocateAsync(...)`는 `Blocked/DeadlineExceeded`로 완료한다. 이를 직접 확인하거나 조작하는 public ACK나
phase API는 제공하지 않는다.

Entry Spot maintenance와 일반 join에서 실행하는 lifecycle callback의 순서, callback 실패 뒤 sealed retry와 whole
User Spot aggregate move의 callback 생략은 [Spot interface](05-spots.ko.md)가 정한다. Actor relocation adapter는 이
lifecycle callback을 대신하지 않는다. 이 순서를 제어하는 public phase API는 없다.

새 distributed Actor를 만들 때 Framework는 여러 target이 같은 Actor를 동시에 만들지 못하도록 생성 권한과
target의 대기 capacity를 함께 예약한다. 이 예약은 다음 순서로 처리한다.

1. Provider는 authority에 `Creating` row를 만들고 target pending capacity를 함께 확보한다.
2. 예약을 먼저 확보한 target만 factory와 Entry Spot의 `OnCreateActorAsync(...)`를
   실행한다.
3. Callback이 승인하면 initial Entry
   [membership](../../../../01-glossary.ko.md#membership), `Ready`, active
   capacity와 `Created` terminal result를 함께 commit한다.
4. Callback이 거절하면 Ready와 active capacity를 만들지 않고 Creating row와 pending
   capacity를 정리하면서 `Rejected` terminal result를 publish한다.
5. Node 종료, timeout 또는 callback exception은 application `Rejected`와 구분한
   `Aborted` failure로 publish한다.
6. 예약 경쟁에서 진 target은 별도 factory를 시작하지 않는다. Provider가 반환한 기존 reservation 결과를
   읽어 현재 생성 시도에 합류한다.

Resolve와 remote messaging은 `Ready` 상태만 사용한다. Entry Spot initialization도 Host `Serving`
publication보다 먼저 완료한다. 이 barrier를 제어하는 application API는 없다.

Actor factory option과 relocation policy는
[Topology configuration](03-configuration-topology.ko.md)의 `AddActorFactory<TActor,TFactory>(...)` configure
callback에서 함께 등록한다. Callback에서 policy를 정확히 하나 선택해야 한다.

Create와 GetOrCreate call은 single-use다. 같은 option을 두 번 설정하면 `InvalidOperation`, terminal
`Async(...)`를 두 번 호출하면 `InvalidOperation`이다. Terminal 호출 시 resolve, reservation, factory와 Ready
barrier 전체에 적용할 deadline 하나를 확정한다. `InMesh(...)`를 생략했을 때 object-role Mesh가 하나이면
그 Mesh를 사용하고, 0개이면 `NotConfigured`, 둘 이상이면 `InvalidOperation`이다. 명시한 Mesh가
없으면 `NotFound`다. Caller는 target RID, predicate와 callback을 지정하지 않는다.

`Create`는 같은 ActorId의 [Ready](../../../../01-glossary.ko.md#ready) incarnation이 있으면 `AlreadyExists`, stable type이 다르면
`TypeMismatch`다. `GetOrCreate`는 같은 type의 Ready Actor를 `Existing`으로
반환하고, Creating attempt이면 authority 변경을 기다린다. CAS loser는 별도 factory를
실행하지 않는다. 서로 다른 operation은 Ready 뒤 `Existing`을 받고 cleanup 뒤 새
reservation을 경쟁하며 앞선 application reply를 공유하지 않는다. 같은 source Node
RID·lifecycle generation·`OperationId`의 재전송만 correlation-free
`creation-operation-terminal-v1` envelope를 읽고 현재 correlation·reply route로 reply를
다시 encode한다. Terminal은 original deadline 뒤 5분 동안 유지한다. Creating attempt가 deadline 안에 끝나지 않으면
해당 caller는 `DeadlineExceeded`다.

Creation request와 semantic terminal envelope는 각각 최대 1 MiB다. Creation request는 reservation 전에 immutable reference와 hash를
기록한다. Factory는 같은 ID, ObjectGeneration과 creation attempt에 대해
retry-safe해야 한다.

`FindAsync(actorId)`는 current Ready `ActorRef`만 반환한다. `FindSpotAsync(actorId)`는 current User Spot
membership의 `SpotRef`만 반환한다. 별도 Actor directory와 public handle·resolver는 제공하지 않는다.
`DestroyAsync(actorRef)`는 exact incarnation만 종료한다. 해당 incarnation이 없으면 `false`, generation이
다르면 `InvalidOperation`, pre-commit seal 중이면 `Unavailable`이며 current ref를 찾아 hidden retry하지
않는다.

`ActorRef.ObjectGeneration`은 1..`long.MaxValue`다. `MeshName`과 `NodeRid`는 조회 시점의 route [snapshot](../../../../01-glossary.ko.md#snapshot)이며
logical identity에는 포함되지 않는다. Relocation 뒤에도 ActorId와 [ObjectGeneration](../../../../01-glossary.ko.md#objectgeneration)은 유지되고 새 location을
가진 ref가 발급된다. 일반 messaging은 ref route를 고정하지 않는다.
