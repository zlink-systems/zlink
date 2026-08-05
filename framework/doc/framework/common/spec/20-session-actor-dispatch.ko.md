---
title: "Session Actor dispatch"
---

# Session Actor dispatch

[스펙 목차](README.ko.md) · [이전: STREAM 서버 session](19-stream-session.ko.md) · [다음: Location runtime](21-location-runtime.ko.md)

> **이 장이 정의하는 것** — STREAM session과 Actor runtime을 연결하는 typed dispatch,
> binding, owner handoff와 실행 순서.


## 1. 이 문서가 정의하는 범위

이 문서는 ZLink Framework에서 STREAM session(연결 하나의 packet 처리와
request correlation을 유지하는 실행 단위)과 Actor runtime을 연결하는
typed dispatch, binding, owner handoff와 실행 순서를 정의한다.

Core raw transport는 Actor identity, 이전 session 작업을 구분하는 binding token,
같은 object incarnation에서 [owner](01-glossary.ko.md#owner)가 바뀐 순서를 나타내는 `AuthorityOwnerGeneration`,
sequence barrier와 Actor route를 해석하지 않는다.

`EnableActorDispatch()`는 `MeshName`을 받지 않고 global object dispatch capability를
활성화한다. Startup은 같은 process에 Object `Client` 또는 `Server` role과 Location
Store가 하나 이상 구성되어 있는지 확인한다.

여러 Mesh가 구성되어 있어도 오류가 아니다. Global ActorId authority가 current
Mesh와 owner를 찾는다. STREAM-only node가 Actor dispatch를 사용하지 않는다면
MeshNode는 필요하지 않다.

Object role이 `None`이거나 Location Store가 없으면 Actor dispatch enablement를 startup에서 거부한다. Hidden
same-process Actor [authority](01-glossary.ko.md#authority)나 local-only binding 의미를 제공하지 않는다.

## 2. Application에 보이는 전체 흐름

Application은 session object, `ActorRef`, typed payload·reply와 bound-session API만
사용한다. Node RID, STREAM transport handle, raw relay envelope, request sequence,
[AuthorityOwnerGeneration](01-glossary.ko.md#authorityownergeneration)과 endpoint를 직접 조립하거나 보관하지 않는다.

1. Session callback이 client를 인증하고 domain Actor identity와 type을 정한다.
2. Global ActorId로 Ready ActorRef를 lookup하거나 application 정책에 따라 Actor를 명시적으로 생성한다.
3. Session과 Actor를 [binding token](01-glossary.ko.md#binding-token)으로 bind한다.
4. Session handler가 typed payload를 current Actor route로 제출한다.
5. Actor handler는 typed reply를 반환하거나 current bound session으로 one-way push를 보낸다.

한 session은 여러 Actor를 동시에 bind할 수 있다. 예를 들어 한 connection이 player
Actor와 party Actor를 함께 사용해도 된다. 반대 방향에는 제한이 있다. Actor 하나는
동시에 session 하나에만 bind할 수 있다. 따라서 session은 Actor별 binding과 route를
각각 보관한다.

| 확인할 질문 | 계약 |
|---|---|
| Session 하나가 bind할 수 있는 Actor 수 | 여러 Actor를 bind할 수 있다. |
| Actor 하나가 동시에 bind할 수 있는 session 수 | 하나다. 새 binding이 확정되면 이전 binding은 무효화한다. |
| Message마다 Actor 위치를 찾는 방법 | Bind 때 확인한 route를 사용한다. Relay할 때 Location Store를 다시 조회하지 않는다. |
| Actor가 다른 node로 이동한 뒤의 route와 위치 | Relocation commit 뒤 Framework가 session에 보관한 해당 Actor route와 bound-session current Actor location snapshot을 target으로 갱신한다. ActorId·ObjectGeneration은 유지한다. |
| Connection이 끊겼을 때 Actor가 알 수 있는 방법 | Framework가 current binding snapshot의 각 Actor에 자동 통지한다. |

## 3. Inbound dispatch와 reply

STREAM packet은 먼저 session의 typed handler registry로 dispatch된다. Handler가
Actor dispatch를 선택하면 Framework는 다음 값을 internal envelope에 보존한다.

- 원본 request correlation
- Binding token
- Actor `ObjectGeneration`
- `AuthorityOwnerGeneration`
- `OwnerLeaseGeneration`: current owner host process lifecycle을 구분한다.
- Session sequence: 현재 session에서 수락한 message의 순서를 나타낸다.

Payload는 local·remote 여부와 관계없이 target Actor application queue에 직접 추가한다. Current Spot은 authority
검증에 사용하지만 callback 실행 문맥이 아니다. Session callback thread에서 Actor handler를 실행하지 않으며 서로
다른 Actor를 session 또는 [Spot](01-glossary.ko.md#spot) global queue로 직렬화하지 않는다.

Request reply·error는 original STREAM correlation을 terminal-once로 완료한다. Request를
target Actor route에 제출한 뒤 timeout, cancellation 또는 route failure가 발생하면 target이 이미 업무를 실행했는지
확정하지 못할 수 있다. Framework는 이런 실패 뒤 다른 Actor, 새 owner 또는 다른
[MeshNode](01-glossary.ko.md#meshnode)를 선택해 같은 request를 자동으로 다시 보내지 않는다.

Session이 닫힌 뒤 늦게 도착한 reply도 새 session이나 새 binding의 reply로 사용하지
않는다. 서로 다른 session의 request가 같은 업무 결과를 공유하는 것을 막기 위한
경계다.

<a id="4-binding-authority"></a>
## 4. Session이 Actor route를 보관하는 방법

Binding은 다음 값을 연결하는 runtime 관계다.

- Exact `ActorRef`의 `ActorId`와 `ObjectGeneration`
- Current `AuthorityOwnerGeneration`과
  [OwnerLeaseGeneration](01-glossary.ko.md#ownerleasegeneration)
- [STREAM session](01-glossary.ko.md#stream-session) identity
- Binding generation과 token. Binding generation은 같은 session owner lifecycle에서
  binding이 교체된 순서를 구분한다.

한 Actor는 동시에 session binding 하나만 가진다. Session 하나에는 여러 Actor를
bind할 수 있다.

Actor direct send/request는 global ActorId가 가리키는 current Ready Actor를 대상으로 하지만,
Session relay는 먼저 current binding token을 확인한다. Actor를 destroy하면 해당 incarnation의
binding도 종료한다. 이후 같은 ActorId로 새 incarnation을 만들어도 이전 binding token은 다시
유효해지지 않는다. 늦게 도착한 relay·unbind·disconnect는 `ObjectGeneration`을 application
message target으로 비교해서가 아니라 종료되거나 교체된 binding identity이기 때문에 거부한다.
Application은 새 `ActorRef`로 bind를 다시 시작해야 한다.

Session owner는 Actor마다 다음 정보를 하나의 binding으로 보관한다.

| 정보 | 사용하는 이유 |
|---|---|
| `ActorId`, `ObjectGeneration` | 같은 ID로 다시 만들어진 다른 Actor에게 보내지 않기 위해 사용한다. |
| `MeshName`, owner `NodeRid` | Bind 이후 relay와 disconnect 통지를 보낼 주소로 사용한다. |
| `NodeGeneration`, `AuthorityOwnerGeneration`, `OwnerLeaseGeneration` | 재시작 전 node나 이전 owner에게 보내지 않기 위해 검증한다. |
| Session owner RID와 lifecycle generation, binding generation과 token | 이전 connection이나 교체된 binding의 늦은 message를 거부한다. |
| Session sequence | 같은 session에서 수락한 message의 순서를 보존한다. |

Bind는 caller가 제출한 `ActorRef`의 위치를 최초 route로 사용해 control request를 한 번 보낸다. Actor와
STREAM session이 서로 다른 MeshNode에 있으면 session owner가
`boundSessionBind(38)` control request를 Actor owner에 보낸다. Actor owner는
Actor `ObjectGeneration`, target `NodeGeneration`(target node process lifecycle을
식별하는 generation)과
`AuthorityOwnerGeneration`을 모두 확인한 뒤
[binding generation](01-glossary.ko.md#binding-generation)을 등록하고 terminal
reply를 한 번만 반환한다.

Session에서 Actor로 들어가는 payload는 등록된 binding generation과
[session sequence](01-glossary.ko.md#session-sequence)를 포함한 `actorSend(24)` record로 Actor owner에 전달한다. Actor가 session에
보내는 push는 `boundSessionSend(36)` record로 session owner에 전달한다. Session
owner는 source Actor `ObjectGeneration`, source `NodeGeneration`,
`AuthorityOwnerGeneration`과 expected binding generation이 모두 current일 때만
실제 STREAM connection에 제출한다.

Source는 bind 전에 Store에서 current route를 미리 조회하지 않는다. Local Actor
instance를 받는 overload도 제공하지 않는다.

Bind가 성공하면 session owner는 검증된 Actor route를 binding에 저장한다. 이후
`RelayAsync(...)`, disconnect 통지와 Actor에서 session으로 보내는 push는 이 binding
정보를 사용한다. Message를 보낼 때마다 Location Store에서 Actor 위치를 조회하지
않는다. 저장한 route가 더 이상 유효하지 않으면 active Message Follow route로 정확히
한 번 전달하거나 `Unavailable`로 끝낸다. Location Store에서 새 `ActorRef`를 찾아
같은 message를 다른 owner에게 자동으로 다시 보내지 않는다.

저장한 route는 current owner lease와 local admission deadline 안에서만 유효하다.
Location Store가 일시적으로 사용할 수 없더라도 이 lease나 deadline을 연장하지
않는다. 따라서 Store 장애가 난 뒤에도 Framework가 새 route를 추측하거나 이전
binding을 무기한 사용하는 동작은 하지 않는다.

Binding identity는 session owner Node RID, 그 node의 lifecycle generation과
owner-local binding generation을 함께 사용한다. Binding generation의 대소 비교는
같은 session owner lifecycle 안에서만 유효하다. 다른 MeshNode가 bind하거나 session
owner가 재시작하면 owner-local counter가 이전 값보다 작더라도 새로운 lifecycle
identity로 등록할 수 있다.

Rebind는 새 identity를 Actor owner와 session owner 양쪽에 등록한 뒤 이전 identity를
무효화한다. Unbind와 disconnect는 `boundSessionBind(38)`의 tombstone transition으로
정확히 해당하는 이전 identity만 제거한다. 이전 owner lifecycle에서 늦게 도착한
push·ingress·close, 이전 Actor `ObjectGeneration`, 이전 authority owner와 재시작 전
`NodeGeneration`은 current binding이나 connection에 적용하지 않는다. 형식이 잘못된
control 및 one-way record는 application queue에 넣지 않으며 one-way record에는 별도
terminal route를 만들지 않는다.

같은 `ObjectGeneration`을 유지하는 Actor relocation의 route와 current location snapshot
갱신은 새 binding identity를 만들지 않으며 rebind가 아니다. Destroy 뒤 같은 ActorId로
새 `ObjectGeneration`이 만들어지면 이전 binding은 유효하지 않으므로 application이 새
`ActorRef`를 명시적으로 bind해야 한다.

다른 owner나 다른 Actor generation으로 rebind할 때 새 Actor owner는 새 identity를
등록한 뒤 이전 exact binding route에 tombstone을 제출한다. 이전 owner가 tombstone을
확인한 뒤에만 새 owner가 bind terminal reply를 반환한다. Session owner는 이 reply를
받기 전까지 기존 binding route를 유지하고, reply를 받은 뒤 새 route로 atomic하게
교체한다. Tombstone 제출이 실패하거나 취소되면 새 bind는 terminal 성공이 아니며
Session owner의 기존 binding도 바뀌지 않는다. 같은 owner에서 새 identity가 이전
identity를 이미 atomic하게 대체한 경우에는 이전 identity tombstone이 새 identity를
제거하지 않는다.

Target에 exact Actor가 없고 active committed Message Follow route가 있으면 original bind control request와 reply
route를 해당 route의 target으로 relay한다. Message Follow route가 없거나 만료됐으면 `Unavailable`, 같은 ActorId의
ObjectGeneration이 다르면 `InvalidOperation`, relocation pre-commit seal 중이면 `Unavailable`로 끝난다.
Source는 Store에서 새 route를 찾아 같은 bind를 hidden retry하지 않는다. `BindOrGet`의 Get은 같은 session의
exact ActorId·[ObjectGeneration](01-glossary.ko.md#objectgeneration) binding만 반환하며 다른 generation이나 directory Actor를 반환하지 않는다.

Binding route는 Framework가 관리한다. Application은 별도 Location row, proxy,
session RID나 endpoint를 만들지 않는다.

Bound-session API는 current binding으로 one-way push를 보내거나 connection close를
요청한다. 임의의 session을 지정하는 global proxy는 제공하지 않는다. Disconnect는
binding을 해제하지만 Actor를 destroy하거나 Spot membership을 바꾸지 않는다.

다음 .NET 발췌는 session이 exact `ActorRef`를 bind하고 payload를 Actor queue로
relay하는 공개 표면을 보여준다. 다른 언어에 같은 signature를 요구하지 않으며,
정확한 .NET 계약은
[.NET STREAM session interface](server/languages/dotnet/interfaces/07-stream-session.ko.md)가
정의한다.

```csharp
public interface IZLinkSessionActors
{
    // 이 session에 현재 bind된 Actor를 모두 제공한다.
    IReadOnlyCollection<IZLinkSessionActor> Bound { get; }

    ValueTask<IZLinkSessionActor> BindAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
    ValueTask<IZLinkSessionActor> BindOrGetAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);

    // Global Actor directory가 아니라 현재 session의 binding만 찾는다.
    IZLinkSessionActor? Find(string actorId);
}

public interface IZLinkSessionActor
{
    ActorRef Ref { get; }
    ValueTask RelayAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken = default);
    ValueTask NotifyDisconnectedAsync(
        CancellationToken cancellationToken = default);
}
```

```csharp
var boundActor = await session.Actors
    .BindAsync(actorRef, cancellationToken); // 이 incarnation과 session을 고정한다.

await boundActor.RelayAsync(
    dispatch,
    payload,
    cancellationToken); // 원래 request 정보와 session sequence를 보존해 제출한다.
```

### 4.1 Connection disconnect를 Actor에 알리는 방법

Framework는 physical connection disconnect를 관찰하면 current binding snapshot을
고정하고 각 exact binding identity에 disconnect를 자동 제출한다. Application의
Session disconnect callback은 bound Actor를 순회하지 않는다. Framework는 binding에
보관한 route와 generation을 검증해 통지를 Actor queue로 전달하며 이 과정에서도
Location Store를 조회하지 않는다.

한 Actor의 제출이나 callback이 실패해도 나머지 Actor 통지와 Session cleanup을
계속하는 all-settled 규칙을 사용한다. Automatic 통지와 public
`NotifyDisconnectedAsync(...)` 논리 통지가 경쟁하면 exact binding identity로
dedupe하고 current Spot의 callback은 최대 한 번 실행한다. Automatic 통지는 lifecycle
deadline 안에서 callback terminal을 기다린 뒤 tombstone과 local cleanup을 진행한다.
Deadline 또는 callback failure가 발생해도 나머지 binding cleanup을 계속한다.

Actor가 속한 현재 Entry Spot 또는 User Spot은 이 통지를
`OnDisconnectActorAsync(...)`로 받는다. Public `NotifyDisconnectedAsync(...)`는
physical connection이 유지된 상태에서 application이 선택한 Actor 하나에 같은 logical
notification을 명시적으로 보내는 operation이다. 이 언어 중립 operation을
`NotifyDisconnected`라 하며 `.NET` interface 문서에서는
`NotifyDisconnectedAsync(...)`로 표현한다. 두 통지는 connection 종료 사실만 알리며
Actor를 destroy하거나 Spot membership을 변경하지 않는다.

```mermaid
sequenceDiagram
    participant Client
    participant SessionOwner as Session owner node
    participant ActorOwner as Actor owner node
    participant ActorQueue as Actor queue
    participant CurrentSpot as Current Spot

    Client->>SessionOwner: 업무 message 전송
    Note over SessionOwner: Bind 때 저장한 Actor route 사용<br/>Location Store 조회 없음
    SessionOwner->>ActorOwner: binding token과 session sequence로 relay
    ActorOwner->>ActorQueue: Actor message 제출

    Client-xSessionOwner: connection 종료
    SessionOwner->>SessionOwner: current binding snapshot 고정
    loop bind된 Actor마다
        SessionOwner->>ActorOwner: 저장한 route로 disconnect 통지
        ActorOwner->>ActorQueue: disconnect 작업 제출
        ActorQueue->>CurrentSpot: OnDisconnectActorAsync 호출
    end
    SessionOwner->>SessionOwner: all-settled 뒤 tombstone과 local cleanup
```

## 5. Actor relocation route barrier

Actor가 다른 MeshNode로 이동해도 physical STREAM connection과 session scope는 session owner process에 유지된다.
Socket, transport handle과 session callback state를 target Actor process로 이동하거나 복제하지 않는다.

Relocation 중에도 session은 Location Store를 조회해 route를 추측하지 않는다. Source와
target이 owner 변경을 commit한 뒤 target이 session owner에 새 route를 전달한다.
Session owner는 요청의 generation과 high-water가 현재 binding에 기록된 값과 같은지 확인한 뒤 해당 Actor binding의
route를 atomic하게 바꾼다. Route switch와 같은 전환에서 bound-session API의 current
ActorRef location snapshot을 target MeshName·NodeRid로 갱신하며 ActorId와
ObjectGeneration은 유지한다. 같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의 route와 physical
STREAM connection은 바꾸지 않는다. 이 위치 갱신은 Target Actor의 message 처리나 Join
completion을 막지 않는다. Application은 relocation을 알기 위해 rebind하지 않는다.

1. Source Actor는 현재 handler가 끝난 뒤 새 Actor message의 application dispatch를 막는다.
   이때 current AuthorityOwnerGeneration, binding generation과 마지막 accepted session sequence를
   기록한다.
2. Seal 전에 Actor queue가 수락한 request와 one-way packet은 reply route와 수락 순서를 포함해
   Relocation Store에 저장한다. Seal 뒤 source로 들어온 Actor message는 크기가 제한된 ingress
   hold에 보관한다.
3. Target은 Restore 요청을 받으면 Actor relocation temporary queue를 등록한다. Relocation
   Store에서 Actor state를 복원하는 동안 들어오는 message는 이 queue에 보관하고 실행하지 않는다.
   Source의 ingress hold message와 이후 이전 route의 message도 temporary queue에 넣는다.
   Restore가 끝나면 owner와 membership을 commit하고 target lifecycle callback을 호출한다.
4. Join으로 이동했다면 target runtime이 Join completion callback을 호출한다. Host relocation의
   `PerActor`와 `SpotWide` 이동에는 Join completion callback이 없으므로 이 단계를 실행하지 않는다.
5. 저장된 기존 작업을 실제 Actor queue에 먼저 넣고 temporary queue의 작업을 그 뒤에 옮긴다.
   Temporary queue 등록을 제거하고 기존 dispatch로 전환한 뒤 Target Actor가 message 처리를
   시작한다. Owner 변경 뒤 source의 이전 route로 도착한 message는 Message Follow가 같은 Actor
   queue에 전달한다.
6. Target runtime은 각 bound Session owner에 `sessionActorLocationUpdateReqMsg`를 send한다.
   이 send의 응답을 기다리기 위해 Target Actor의 처리를 중단하지 않는다.
7. Session owner는 요청의 Actor ObjectGeneration이 현재 binding과 같은지 확인하고, 이전·target
   AuthorityOwnerGeneration,
   binding generation, session owner lease와 high-water를 검증한다. 검증에 성공하면 해당
   Actor route와 bound-session current Actor location snapshot을 atomic하게 바꾸고
   `sessionActorLocationUpdateResMsg`를 send한다. Snapshot은 같은
   ActorId·ObjectGeneration과 target MeshName·NodeRid를 가진다.
8. Target runtime은 응답이 없으면 첫 요청을 보낸 지 1초 뒤 같은 요청을 다시 보낸다.
   이후 재전송 간격은 1초, 2초, 4초, 5초이며 그 뒤에는 5초를 유지한다.

Route 갱신은 binding이 가리키는 `ObjectGeneration`과 같은 Actor relocation에만
허용한다. 같은 ActorId라도 새 incarnation이 만들어졌다면 Framework가 기존 binding을
그 새 Actor로 바꾸지 않으며, application이 새 `ActorRef`로 명시적인 bind를 시작해야
한다. 같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor는 route·location snapshot·token·generation을 유지한다.

### 5.1 Session Actor 위치 갱신 message

Target runtime이 relocation된 Actor의 새 위치를 Session owner에 반영하고 응답을 받을
때까지 재시도하는 전체 작업을 `sessionRelocationRouteUpdate`라고 한다. 이 작업은 Target
Actor의 실행과 독립적으로 진행한다.

`sessionActorLocationUpdateReqMsg`와 `sessionActorLocationUpdateResMsg`는 transport의
동기 request/reply가 아니다. Target runtime과 Session owner가 각각 send하는 두 개의
infrastructure message다. `ReqMsg`와 `ResMsg` 이름은 어느 message가 위치 갱신을 요청하고
어느 message가 처리 결과를 반환하는지 구분한다.

`sessionActorLocationUpdateReqMsg`에는 relocation ID, ActorId, ObjectGeneration,
이전·target AuthorityOwnerGeneration, target MeshName·NodeRid, Session owner identity,
SessionRid, binding generation과 마지막 accepted session sequence를 넣는다. Session owner는
이 값으로 현재 binding과 같은 Actor relocation인지 확인한 뒤 binding route와 current
`ActorRef` 위치 snapshot을 한 번에 바꾼다.

`sessionActorLocationUpdateResMsg`에는 요청과 같은 relocation ID, SessionRid, ActorId,
ObjectGeneration, binding generation과 다음 처리 결과를 넣는다.

| 값 | 결과 | 의미 |
|---:|---|---|
| 0 | `Applied` | 요청한 route와 location snapshot을 이번에 갱신했다. |
| 1 | `AlreadyApplied` | 같은 relocation의 갱신을 이미 적용했다. |
| 2 | `Stale` | 더 최신 binding generation, owner generation 또는 Actor 위치가 적용되어 있다. |
| 3 | `SessionOrBindingClosed` | 대상 Session 또는 binding이 종료되어 갱신할 수 없다. |

Session owner는 요청을 처리할 수 있으면 결과와 함께 응답한다. Target runtime은 네 결과
중 하나를 받으면 해당 요청의 재전송을 중단한다. Source Message Follow route도 이 응답을
받거나 `MessageFollowDuration`이 끝나면 제거한다. `Stale`와
`SessionOrBindingClosed`는 이전 위치를 다시 적용하지 않았음을 뜻한다.

응답이 없으면 Target runtime은 같은 relocation ID와 binding generation을 가진 요청을
정해진 간격으로 다시 보낸다. 첫 재전송은 최초 send 후 1초 뒤에 수행한다. 이후에도 응답이
없으면 1초, 2초, 4초, 5초 간격으로 다시 보내며 그 뒤에는 5초 간격을 유지한다. Session
owner는 같은 요청을 여러 번 받아도 route와 snapshot을 한 번 갱신한 것과 같은 결과를
유지하고 매번 같은 처리 결과를 응답해야 한다. 위치 갱신이 확인되기 전에는 source Message
Follow route가 `MessageFollowDuration` 안에서 이전 route로 도착한 message를 Target Actor에
전달한다. Route가 만료되면 이전 route의 request는 `Unavailable`로 끝나지만 위치 갱신
재전송은 실행 중인 target runtime이 계속한다. Target runtime이 종료되면 다른 runtime이
같은 요청을 자동으로 이어서 보내지 않는다. 재전송은 Join completion, Target Actor의
message 처리나 source host의 Shutdown을 지연시키지 않는다.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceActor as Source Actor
    participant RelocationStore as Relocation Store
    participant LocationStore as Location Store
    participant TargetRuntime as Target runtime
    participant TargetTemp as Actor temporary queue
    participant TargetQueue as Target Actor queue
    participant TargetActor as Target Actor
    participant SessionOwner as Session owner

    SourceRuntime->>SourceActor: 현재 handler 종료 뒤 application dispatch 중단
    SourceRuntime->>RelocationStore: Actor state와 저장된 기존 작업 기록
    SourceRuntime->>TargetRuntime: Actor Restore 요청
    TargetRuntime->>TargetTemp: Actor temporary queue 등록
    TargetRuntime->>RelocationStore: Actor state와 저장된 queue 읽기
    TargetRuntime->>TargetActor: Actor 생성과 state Restore
    SourceRuntime->>TargetRuntime: ingress hold message relay
    TargetRuntime->>TargetTemp: temporary queue에 message 보관
    TargetRuntime->>LocationStore: target owner와 membership commit
    TargetRuntime->>TargetActor: lifecycle·Join completion callback
    TargetRuntime->>TargetQueue: 저장된 기존 작업 먼저 추가
    TargetRuntime->>TargetQueue: temporary queue 작업 이동
    TargetRuntime->>TargetTemp: 등록 제거 후 기존 dispatch로 전환
    TargetQueue->>TargetActor: message 처리 시작
    TargetRuntime-)SessionOwner: sessionActorLocationUpdateReqMsg send
    SessionOwner->>SessionOwner: generation 확인 후 route와 current ActorRef snapshot 교체
    SessionOwner-)TargetRuntime: sessionActorLocationUpdateResMsg send
    Note over TargetRuntime,SessionOwner: ResMsg가 없으면 같은 ReqMsg를 1초, 1초, 2초, 4초, 이후 5초 간격으로 재전송
```

이 다이어그램은 relocation commit 뒤 session route를 target Actor로 전환하는 정상
경로다. Physical STREAM connection은 Session owner에 남는다. Target Actor는 위치 갱신
응답을 기다리지 않고 message를 처리하며, 이전 route로 도착한 message는 source Message
Follow route를 통해 받는다.

Session Actor 위치 갱신 상태는 Target Actor의 message 처리를 제어하지 않는다. 이전 owner,
stale authority owner generation, binding token과 sequence의 packet·reply·push·close는 current
connection에 적용하지 않는다.

## 6. Failure 처리

Commit 전 failure에서는 Session Actor 위치 갱신을 보내지 않는다. Session owner의 binding route와
current `ActorRef` 위치 snapshot은 source를 계속 가리킨다. Framework는 Location Store에서 source가
owner로 유지되는지 확인하고 target temporary queue를 폐기한 뒤 source Actor queue와 ingress를
복원한다. Owner를 확인하기 전에는 source Actor의 message 처리를 다시 시작하지 않는다.

Commit 뒤에는 source route나 location snapshot으로 rollback하지 않는다. 실행 중인 current
target만 `sessionActorLocationUpdateReqMsg` 재전송을 이어간다. Session owner가 위치
갱신을 확인하지 못했더라도 source Message Follow route는 `MessageFollowDuration`까지만 이전
route로 도착한 message를 target에 전달한다.
Session owner process가 종료되면 connection을 다른 process로 복구하지 않고 닫으며 client reconnect가 새 session을 만든다.

Physical disconnect는 accepted participant high-water, request terminal completion 또는 relocation cleanup의 증거가
아니다. Session에서 Actor로 전달한 요청도 다른 Actor 요청과 같은 규칙을 따른다. Seal 전에 Actor queue가
수락한 요청은 저장한 기존 작업에 포함하고, seal 뒤 owner commit 전에 source로 들어온 요청은 ingress
hold에서 target temporary queue로 relay한다.

## 7. Execution과 lifecycle

같은 session의 handler turn, binding mutation, close와 relocation barrier는 session owner가 직렬화한다. Actor에
제출한 뒤에는 Actor queue가 순서를 소유한다. Session turn과 Actor turn을 shared lock이나 callback stack으로 합치지
않는다.

Request completion, send-ready, binding update, relocation barrier와 disconnect cleanup은 infrastructure task에서
진행한다. Session 또는 Actor application callback이 비동기 작업을 기다리는 동안에도 진행해야 한다.

Actor owner host의 Relocate는 §5 barrier를 사용한다. Session owner host의 Relocate와 Shutdown은 신규 session·binding을
거부하고 accepted callback·reply·cleanup을 [deadline](01-glossary.ko.md#deadline)까지 처리한 뒤 connection을 닫는다. Physical connection을
다른 process로 이동하지 않는다.

## 8. Startup과 operation error

| Condition | Result |
|---|---|
| Object `Client`·`Server` role이 없다. | Configuration error로 startup에 실패한다. |
| [Location Store](01-glossary.ko.md#location-store)가 없다. | Configuration error로 startup에 실패한다. |
| `ActorRef` 위치가 stale하고 Message Follow route도 없다. | `Unavailable`로 끝난다. |
| `ObjectGeneration`이 다르다. | `InvalidOperation`으로 끝난다. |
| Actor가 relocation pre-commit seal 상태다. | `Unavailable`로 끝난다. |
| 같은 packet key의 handler를 중복 등록했다. | Configuration error로 startup에 실패한다. |
| Actor factory가 없다. | Explicit create error로 끝난다. |
| Current binding 없이 push 또는 close를 요청했다. | Session-not-bound 오류로 끝난다. Public kind는 `InvalidOperation`이다. 대상이 없는 것이 아니라 binding을 먼저 만들어야 하는 순서 문제이며, binding이 생기면 같은 호출이 성공한다. |
| Actor·owner·binding fence가 stale하다. | Typed stale error로 끝나며 다른 대상으로 fallback하지 않는다. |

## 9. 구현 및 contract test 검증 요구

- Actor dispatch enablement가 MeshName을 받지 않고 global Actor authority를 사용한다.
- Object role 또는 Store가 없으면 startup에서 거부하고 local-only binding을 만들지 않는다.
- Session 하나에 여러 Actor를 bind하고 Actor마다 독립된 route와 binding token을 유지한다.
- Local·remote payload가 Actor queue로 직접 전달되고 Spot callback을 거치지 않는다.
- Bind가 exact ActorRef를 한 번 제출하고 stale route를 hidden Store retry하지 않는다.
- Bind 뒤 relay와 disconnect 통지가 저장한 route를 사용하며 message마다 Location Store를
  조회하지 않는다.
- Physical disconnect 때 Framework가 current binding snapshot 전체에 자동 all-settled 통지하고 current
  Spot의 `OnDisconnectActorAsync(...)`를 exact binding identity마다 최대 한 번 호출한다.
- Rebind 뒤 이전 token과 authority fence가 current binding을 바꾸지 않는다.
- 두 node 사이의 bind, session ingress와 Actor push가 각각 command 38,
  bound-session tail을 포함한 command 24, command 36의 raw ROUTER 경로를
  사용한다.
- Request reply가 original STREAM correlation으로 한 번 완료된다.
- Physical STREAM connection과 session object를 Actor target process로 이동하지 않는다.
- Relocation commit 뒤 Target Actor가 message 처리를 시작하며 session owner의 해당 Actor
  route와 bound-session current Actor location snapshot은 비동기 send message로 갱신된다.
- Bound-session request도 수락 시점에 따라 저장한 기존 작업 또는 ingress hold relay에 포함된다.
- 위치 갱신 응답이 없어도 Join completion과 Target Actor message 처리가 지연되지 않으며,
  정해진 재전송 간격을 적용한다. Message Follow route는 `MessageFollowDuration` 뒤 제거하고
  실행 중인 target runtime만 위치 갱신 재전송을 이어가야 한다.
- Commit 전 failure는 source route를 복원한다. Commit 뒤에는 source로 rollback하거나 다른
  runtime이 자동 복구하지 않는다.
