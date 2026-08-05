# Java Actor 공개 인터페이스

Session에 bind된 Actor를 포함한 Spot relocation은 target에서 Actor와 queue를 복원하고 owner와
membership을 commit한 뒤 message 처리를 시작한다. Target runtime은
`sessionActorLocationUpdateReqMsg`를 send하여 binding route와 bound-session current Actor
location snapshot을 갱신한다. 응답이 없어도 Actor 처리를 멈추지 않으며 정해진 간격으로
같은 요청을 다시 보낸다. Snapshot은 target MeshName·NodeRid를 제공한다. Relocation 자체는 physical·logical disconnect가
아니므로 Actor disconnect callback을 실행하지 않는다. relocation 대상에 포함되지 않은 다른 Actor의 route와 physical connection은
변경하지 않는다.

[인터페이스 목차](README.ko.md) · [Actor 공통 계약](../../../../14-actor-model.ko.md)

이 문서는 Java에서 Actor factory, context, messaging, manager와 relocation adapter를 표현하는 공개
인터페이스를 고정한다. 일반 message는 ActorId로 대상을 지정하고, 특정 incarnation을 변경하는
operation은 exact `ActorRef`를 사용한다.

```java
public interface ZLinkActorFactory {
    CompletionStage<ZLinkActor> create(ZLinkActorContext context);
}

public interface ZLinkActorHandlerRegistry {
    void addHandler(Class<?> handlerType);
}

public interface ZLinkRelocationCancellation {
    boolean isCancellationRequested();
}

public interface ZLinkActorRelocationAdapter<TActor extends ZLinkActor> {
    CompletionStage<byte[]> capture(
        TActor actor, ZLinkRelocationCancellation cancellation);
    CompletionStage<Void> restore(
        TActor actor, byte[] state, ZLinkRelocationCancellation cancellation);
}

```

[Factory](../../../../01-glossary.ko.md#factory) registration의 정확한 builder member는
[구성과 host](configuration-host.ko.md)가 소유한다. Cross-node relocation 동작은 Actor factory configure callback에
직접 연결한다. Runtime은 factory가 반환한 Actor를 명시한 `actorClass`로 검사해 type 불일치를 startup
오류로 반환한다. Factory와 분리된 relocation registry는 제공하지 않는다.
`preserveStateWith(...)`의 `adapterClass`는 해당 Actor type의
`ZLinkActorRelocationAdapter<TActor>`를 구현해야 한다. User·Instance Spot policy의 adapter type 검증은
[Spot 인터페이스](spots.ko.md)가 소유한다. `Class<?>`를 받는 것은 Java type erasure 때문에 policy value를
공통으로 유지하기 위한 표현이며, Framework는 factory type과 adapter generic target이 일치하는지 socket bind
전에 검사한다. Mismatch는 startup configuration error다.
`preserveStateWith(null)`은 callback 실행 중 configuration error로 거부한다.

Actor adapter는 application state를 최대 64 MiB의 opaque `byte[]`로 capture·restore한다. Public state DTO, `TState`,
`stateContractId`, state class와 `ZLinkMessage`를 relocation surface에 두지 않는다. Framework는 capture가 정상
완료한 배열을 즉시 복사한다. Capture가 반환한 배열은 adapter가 계속 소유하며 completion 뒤 재사용하거나
변경해도 저장 payload가 바뀌지 않는다. Restore에는 호출마다 저장 payload의 fresh defensive copy를 전달하고
adapter는 stage가 끝난 뒤 그 배열을 보관하지 않는다. 길이가 0인 배열도 유효한 보존 state이며
`recreateOnRelocation()`을 선택한 것으로 해석하거나 restore를 생략하지 않는다. Adapter는 owner claim, relocation envelope, generation과 recovery phase를
받지 않는다.

Cross-node materialization에서 Actor factory가 `preserveStateWith(...)`를 사용하면 maintenance Actor relocation,
remote User·[Entry Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) join과 whole User Spot relocation의 각 Actor participant에 같은 Actor adapter를 사용한다.
Same-node join과 `disableRelocation()` 또는 `recreateOnRelocation()`을 선택한 factory에서는 adapter를 호출하지
않는다. `recreateOnRelocation()`은 application state를 capture하지 않으므로 adapter가 없다.

Target은 restore와 accepted journal staging을 끝낸 뒤 owner를 commit한다. Lifecycle callback 뒤 저장된 기존
작업을 실제 Actor queue에 먼저 넣고 relocation temporary queue 작업을 그 뒤에 옮긴다. Temporary queue
등록을 제거하고 dispatch를 atomic하게 전환한 뒤 target을 `READY`로 연다. Source cleanup, `COMPLETED` 기록과
bound-session 위치 갱신 응답은 target message 처리를 막지 않는다. `READY` 뒤 target process가 종료되면
ordinary [owner](../../../../01-glossary.ko.md#owner) loss로 처리하며 이전 relocation을 자동 replay하지 않는다. 이 barrier를 조작하는 public phase API는 제공하지 않는다.

같은 source와 target process 안의 재시도에서 factory와 `restore(...)`를 두 번 이상 호출할 수 있다.
`capture(...)`도 [authority](../../../../01-glossary.ko.md#authority) commit 전에 반복될 수 있다. Current owner와 attempt fence만 completion을 commit하고
admission을 열 수 있다. Callback에는 relocation ID를 추가하지 않으므로 application restore와 capture는 retry-safe해야
하며 exactly-once external side effect를 보장하지 않는다. Factory는 target attempt마다 fresh Actor instance를
만들고 Framework는 그 attempt의 `restore(...)`만 해당 instance에 호출한다. Source instance나 이전 target
attempt의 instance를 새 attempt에 재사용하지 않으며 같은 attempt에서는 restore가 반복될 수 있다.

Capture stage가 exception으로 끝나면 authority publication 전에 attempt를 abort하고 source authority와 admission을
유지한다. Restore stage가 exception으로 끝나면 target admission을 sealed 상태로 유지하고 같은 target process에서
동일한 payload로 다시 시도할 수 있다. 다른 target을 자동 선택하지 않는다. Exception을 빈 payload나 정상 completion으로 바꾸지 않는다. Capture의
null stage와 null `byte[]`, Restore의 null stage는 adapter contract 위반이다. Host relocation에서 deadline이 먼저
확정되지 않은 precommit adapter exception과 contract 위반은 `Blocked/StateIncompatible`로 분류한다. [Deadline](../../../../01-glossary.ko.md#deadline)이
먼저 확정되면 `Blocked/DeadlineExceeded`를 사용하며 stale target attempt의 cancellation은 terminal result를
commit하지 못한다. Adapter는
반복 호출과 stale attempt overlap을 허용하도록 retry-safe해야 하며 callback 안의 외부 side effect를 exactly-once로
간주할 수 없다.

Relocated terminal reply accounting은 internal command ID 46 `replyRelayAck`를 사용한다. 이 command는 stable
relocation ID, operation ID, exact request-source fence(owner ID, lease generation, node RID, node generation)와
status만 가지며 payload와 metadata를 싣지 않는다. Physical connection close는 terminal 증거가 아니다. ACK 또는
accepted record에 저장한 exact request-source lease expiry만 terminal accounting을 완료하며 public ACK API는 없다.

Source는 connection-bound one-way를 포함해 admission한 모든 connection-bound work가 terminal accounting에
도달한 뒤에만 `CAPTURED`를 commit한다. Durable accepted journal은 exact owner lease가 있는 source에서만
사용한다. Pre-`CAPTURED` drain이 deadline 안에 끝나지 않으면 relocation을 abort하고 host relocation을
`BLOCKED/DEADLINE_EXCEEDED`로 끝낸다. Durable abort와 source normalization이 끝나기 전에 source admission을
열지 않는다. Connection-bound one-way를 미완료 상태로 capture하는 예외는 없다.

Entry Spot과 `PerActor` User Spot의 Actor는 독립된 relocation unit이다. `SpotWide`
User Spot member Actor만 Spot과 current member 전체를 하나의 aggregate로 함께
옮긴다. User Spot membership 자체는 relocation blocker가 아니며 participant 하나라도
`disableRelocation()`을 선택했거나 호환 target을 확보할 수 없을 때만 해당 Actor unit 또는
`SpotWide` aggregate를 차단한다. Relocation을 비활성화한 participant는
`BLOCKED/RELOCATION_DISABLED`, target·capacity·reservation 부재는
`BLOCKED/TARGET_UNAVAILABLE`, application version·type·state 보존 adapter capability 불일치는
`BLOCKED/STATE_INCOMPATIBLE`다. Actor unit은 target factory와 restore를 끝내고 accepted journal을
application handler가 실행하지 않은 staging queue로 준비한 뒤 `NEW_OWNER` CAS를 수행한다. 이 CAS는
owner, authority owner generation과 current [Spot](../../../../01-glossary.ko.md#spot)을
target execution shell로 원자적으로 바꾼다. Infrastructure relocation은 application
membership callback을 호출하지 않는다. Journal·queue·Actor timer replay, source
relay와 durable cleanup을 끝낸 뒤 dispatch를 개방한다. 이 순서를 제어하는 public
phase API는 없다.

새 distributed Actor를 만들 때 Framework는 owner가 될 target 하나를 선택하고, 그 target에서
`CREATING` authority와 pending capacity를 하나의 reservation으로 함께 확보한다. Reservation을 확보한
target만 factory, initial Entry membership과 initialize를 수행한다. 성공하면 같은 reservation을 `READY`와
active capacity로 commit하고 실패하면 abort한다. CAS 경쟁에서 진 target은 별도 factory를 실행하지 않는다.

Actor Join call은 동기 `defer()`만 제공하며 `submit(...)`·`yield(...)`를 제공하지
않는다. `defer()`는 current handler에 immutable Join intent와 비활성 barrier만
등록하며 target 조회나 Store I/O를 시작하지 않는다. Handler가 정상적으로 끝나면
Join을 실행하고 실패하면 barrier를 폐기한다. 결과는 같은 128-bit operation ID의
`onJoinCompleted(...)` Actor callback으로 전달한다.

Operation ID는 completion idempotency ID이며 `RelocationId`, reservation ID나
aggregate commit ID가 아니다. Same-node와 cross-node completion retry는 current
source와 target process lifetime으로 제한한다. Process 종료 뒤 다른 runtime이
completion을 자동 replay하지 않는다.

Request 없는 overload는 empty `ZLinkMessage`를 고정한다. Timeout 기본값은 5초이고
명시 값은 millisecond 올림 기준 유한한 `1..Integer.MAX_VALUE` ms다. `defer()`에서
monotonic absolute deadline을 고정한다.

## Exact public member inventory

아래 선언은 이 category의 Java public type과 member를 고정한다.

```java
public interface systems.zlink.framework.actors.ZLinkActorFactory {
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActor> create(systems.zlink.framework.actors.ZLinkActorContext);
}
public interface systems.zlink.framework.actors.ZLinkRelocationCancellation {
  public abstract boolean isCancellationRequested();
}
public interface systems.zlink.framework.actors.ZLinkActorRelocationAdapter<TActor extends systems.zlink.framework.actors.ZLinkActor> {
  public abstract java.util.concurrent.CompletionStage<byte[]> capture(TActor, systems.zlink.framework.actors.ZLinkRelocationCancellation);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> restore(TActor, byte[], systems.zlink.framework.actors.ZLinkRelocationCancellation);
}
public interface systems.zlink.framework.actors.ZLinkActorHandlerRegistry {
  public abstract void addHandler(java.lang.Class<?>);
}
public final class systems.zlink.framework.actors.ActorRef extends java.lang.Record {
  public systems.zlink.framework.actors.ActorRef(java.lang.String, long, java.lang.String, systems.zlink.contracts.core.RoutingId);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public java.lang.String actorId();
  public long objectGeneration();
  public java.lang.String meshName();
  public systems.zlink.contracts.core.RoutingId nodeRid();
}
public interface systems.zlink.framework.actors.ZLinkActor {
  public abstract systems.zlink.framework.actors.ZLinkActorContext context();
  public default void configure();
  public default java.util.concurrent.CompletionStage<java.lang.Void> onJoinCompleted(systems.zlink.framework.actors.ZLinkActorJoinCompletion);
}
public interface systems.zlink.framework.actors.ZLinkActorClient {
  public abstract systems.zlink.framework.actors.ZLinkActorSendCall sendToActor(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorRequestCall requestToActor(java.lang.String, java.lang.Object);
}
public interface systems.zlink.framework.actors.ZLinkActorContext {
  public abstract java.lang.String actorId();
  public abstract long objectGeneration();
  public abstract java.lang.String meshName();
  public abstract java.util.Optional<java.lang.String> spotId();
  public abstract systems.zlink.framework.actors.ZLinkBoundSession boundSession();
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinSpot(java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinSpot(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinEntrySpot();
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinEntrySpot(java.lang.Object);
}
public interface systems.zlink.framework.actors.ZLinkActorJoinCall {
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall timeout(java.time.Duration);
  public abstract void defer();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinOperationId extends java.lang.Record {
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId(long, long);
  public long high();
  public long low();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinCompletion$Accepted extends java.lang.Record implements systems.zlink.framework.actors.ZLinkActorJoinCompletion {
  public systems.zlink.framework.actors.ZLinkActorJoinCompletion$Accepted(systems.zlink.framework.actors.ZLinkActorJoinOperationId, systems.zlink.framework.actors.ActorRef, systems.zlink.framework.messaging.ZLinkMessage);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId();
  public systems.zlink.framework.actors.ActorRef actor();
  public systems.zlink.framework.messaging.ZLinkMessage reply();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinCompletion$Rejected extends java.lang.Record implements systems.zlink.framework.actors.ZLinkActorJoinCompletion {
  public systems.zlink.framework.actors.ZLinkActorJoinCompletion$Rejected(systems.zlink.framework.actors.ZLinkActorJoinOperationId, systems.zlink.framework.messaging.ZLinkMessage);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId();
  public systems.zlink.framework.messaging.ZLinkMessage reply();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinCompletion$Failed extends java.lang.Record implements systems.zlink.framework.actors.ZLinkActorJoinCompletion {
  public systems.zlink.framework.actors.ZLinkActorJoinCompletion$Failed(systems.zlink.framework.actors.ZLinkActorJoinOperationId, systems.zlink.framework.errors.ZLinkFrameworkErrorKind);
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId();
  public systems.zlink.framework.errors.ZLinkFrameworkErrorKind kind();
}
public sealed interface systems.zlink.framework.actors.ZLinkActorJoinCompletion
    permits systems.zlink.framework.actors.ZLinkActorJoinCompletion.Accepted,
            systems.zlink.framework.actors.ZLinkActorJoinCompletion.Rejected,
            systems.zlink.framework.actors.ZLinkActorJoinCompletion.Failed {
}
public interface systems.zlink.framework.actors.ZLinkActorManager {
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall create(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall getOrCreate(java.lang.String, java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.util.Optional<systems.zlink.framework.actors.ActorRef>> find(java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.util.Optional<systems.zlink.framework.spots.SpotRef>> findSpot(java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.lang.Boolean> destroy(systems.zlink.framework.actors.ActorRef);
}
public interface systems.zlink.framework.actors.ZLinkActorCreateCall {
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall inMesh(java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall request(java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall request(systems.zlink.framework.messaging.ZLinkMessage);
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall timeout(java.time.Duration);
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> submit();
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> yield();
}
public interface systems.zlink.framework.actors.ZLinkActorGetOrCreateCall {
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall inMesh(java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall request(java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall request(systems.zlink.framework.messaging.ZLinkMessage);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall timeout(java.time.Duration);
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> submit();
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> yield();
}
public sealed interface systems.zlink.framework.actors.ZLinkActorCreateResult
    permits systems.zlink.framework.actors.ZLinkActorCreateResult.Existing,
            systems.zlink.framework.actors.ZLinkActorCreateResult.Created,
            systems.zlink.framework.actors.ZLinkActorCreateResult.Rejected {
}
public interface systems.zlink.framework.actors.ZLinkActorRequestCall {
  public abstract systems.zlink.framework.actors.ZLinkActorRequestCall metadata(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorRequestCall timeout(java.time.Duration);
  public abstract <TReply> java.util.concurrent.CompletionStage<TReply> submit(java.lang.Class<TReply>);
  public abstract <TReply> java.util.concurrent.CompletionStage<TReply> yield(java.lang.Class<TReply>);
}
public interface systems.zlink.framework.actors.ZLinkActorSendCall {
  public abstract systems.zlink.framework.actors.ZLinkActorSendCall metadata(java.lang.String, java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.actors.ZLinkBoundSession {
  public abstract systems.zlink.framework.actors.ZLinkBoundSessionSendCall send(java.lang.Object);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> disconnect();
}
public interface systems.zlink.framework.actors.ZLinkBoundSessionSendCall {
  public abstract systems.zlink.framework.actors.ZLinkBoundSessionSendCall metadata(java.lang.String, java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
```

ActorId는 UTF-8 1..255 bytes의 global logical ID다. `ActorRef`는 ActorId, positive signed-63-bit
ObjectGeneration과 조회 시점의 MeshName·NodeRid를 보존한다. 일반 message는 ActorId만 받고 current authority를
resolve한다. Destroy와 session bind만 exact ref를 받는다.

Create와 GetOrCreate call은 single-use다. 같은 option을 두 번 설정하거나 submit을 두 번
호출하면 `INVALID_OPERATION`이다. `inMesh` 생략 시 object-role Mesh가 하나면 자동 선택하고 0개이면
`NOT_CONFIGURED`, 둘 이상이면 `INVALID_OPERATION`이다. 명시한 Mesh가 없으면
`NOT_FOUND`다. Caller는 target RID나 placement callback을 지정하지 않는다. `find`와 `findSpot`은
current Ready ref만 반환하며 directory와 resolver를 제공하지 않는다.

`create`는 Ready Actor가 있으면 `ALREADY_EXISTS`이며 새 attempt에서는 `Created`
또는 `Rejected`를 반환한다. `getOrCreate`는 같은 type의 Ready Actor를 callback 없이
`Existing`으로 반환한다. Creating이면 authority 변경을 기다리며 CAS loser는
별도 factory나 callback을 시작하지 않는다. 서로 다른 operation은 Ready 뒤 `Existing`을
받고 cleanup 뒤 새 reservation을 경쟁하며 앞선 application reply를 공유하지 않는다.
같은 source Node RID·lifecycle generation·`OperationId`의 재전송만 correlation-free
`creation-operation-terminal-v1` envelope를 읽고 현재 correlation·reply route로 reply를
다시 encode한다. Terminal은 original deadline 뒤 5분 동안 유지한다. Callback exception은 `Rejected`가 아니라
typed creation failure다.

`ActorRef.objectGeneration()`은 `1..Long.MAX_VALUE`다. Typed JSON은 required property `actorId`,
`objectGeneration`, `meshName`, `nodeRid`를 사용하며 generation은 leading-zero 없는 decimal string으로 encode한다.
Unknown property, duplicate property, required property 누락, 숫자 token과 범위 밖 값은 거부한다.

Actor request에 선언된 `yield(...)`는 현재 Actor handler가 `SpotWide` User Spot의 shared execution
gate에서 실행 중일 때만 유효하다. Entry Spot Actor와 `PerActor` User Spot의 Actor가 호출하면 operation을
제출하거나 turn을 반환하지 않고 `INVALID_OPERATION`으로 완료한다. Actor Join은 현재
handler 안에서 `defer()`로만 등록하며 `submit(...)`과 `yield(...)`를 제공하지 않는다.
