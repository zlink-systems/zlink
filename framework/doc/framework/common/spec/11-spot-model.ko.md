---
title: "Spot 모델 — Entry, User, Instance"
---

# Spot 모델 — Entry, User, Instance

[스펙 목차](README.ko.md) · [이전: Network listener identity](10-network-listener-identity.ko.md) · [다음: SPOT 메시징](12-spot-messaging.ko.md)

> **이 장이 정의하는 것** — Entry Spot, User Spot, Instance Spot의 공통점과 차이점.


## 1. 범위

이 문서는 Framework가 제공하는 Entry Spot, User Spot과 Instance Spot의 공통점과
차이점을 정의한다. 세 종류 모두 주소와 상태를 가지고 순서대로 callback을 실행하는
[Spot](01-glossary.ko.md#spot)이지만 생성 목적, [Actor membership](01-glossary.ko.md#membership),
종료와 relocation 계약은 서로 다르다.

이 문서는 “어떤 Spot 종류를 사용해야 하는가?”와 “Entry Spot이 어떤 역할을
하는가?”에 답한다. Message 전달 방법은 [20 Spot 메시징](12-spot-messaging.ko.md),
Actor callback의 정확한 순서는 [23 Spot과 Actor membership](15-spot-actor.ko.md),
User·Instance Spot의 생성과 주소 계약은
[24 Spot 주소 메시징](16-spot-address-messaging.ko.md)이 소유한다.

## 2. 세 Spot은 준비되는 시점과 목적이 다르다

```mermaid
flowchart LR
    Server["Object Server startup"] -->|"등록한 Entry Spot 초기화"| Entry["Entry Spot<br/>Actor의 기본 membership"]
    Manager["Application manager call"] -->|"Create 또는 GetOrCreate"| User["User Spot<br/>Application이 관리하는 Actor container"]
    Message["Instance intent를 가진 첫 message"] -->|"대상이 Missing이면 준비"| Instance["Instance Spot<br/>Actor가 없는 message 처리 단위"]
```

Entry Spot은 Object Server와 함께 준비된다. User Spot은 application이 manager로
명시적으로 만들며, Instance Spot은 별도 create operation 없이 첫 direct message가
필요할 때 준비된다.

## 3. 공통점과 차이점

| 구분 | Entry Spot | User Spot | Instance Spot |
|---|---|---|---|
| 주된 목적 | 해당 Object Server에 배치된 Actor의 초기·기본 membership을 관리한다. | Application이 명시적으로 만드는 Spot이며 Actor membership을 관리할 수 있다. | Actor 없이 direct message와 timer를 처리한다. |
| 등록·생성 | Object Server builder에 Spot 구현 type을 등록하고 startup에서 초기화한다. | Stable type의 factory를 등록하고 manager `Create`·`GetOrCreate`로 만든다. | Stable type의 factory를 등록하고 Instance intent를 가진 첫 direct call로 준비한다. |
| Spot ID | Framework가 발급한다. Caller가 fixed Spot ID를 지정하지 않는다. | `Create`는 Framework가 발급하고 `GetOrCreate`는 caller가 지정한다. | Caller가 direct message의 target Spot ID를 지정한다. |
| Stable type 입력 | 별도 stable-type 문자열을 등록하지 않는다. | UTF-8 1..255 byte stable type이 필수다. | UTF-8 1..255 byte stable type을 사용한다. Missing activation에서는 명시하거나 유일한 등록 type을 선택한다. |
| Actor membership | 지원한다. Actor 생성의 initial membership이며 `JoinEntrySpot`의 대상이다. | 지원한다. Actor가 `JoinSpot`과 leave로 membership을 변경할 수 있다. | 지원하지 않는다. |
| Direct packet | 지원한다. | 지원한다. | 지원한다. |
| Timer와 outbound call | 지원한다. | 지원한다. | 지원한다. |
| 기본 application 실행 | Spot handler와 timer는 Spot turn에서 직렬화하고 Actor는 Actor별로 실행한다. | `SpotWide`: Spot·member Actor·timer·lifecycle callback 전체를 직렬화한다. | Direct handler와 timer를 Spot 전체에서 직렬화한다. |
| Optional 실행 방식 | 제공하지 않는다. | `PerActor`: Actor별, Spot lane별, timer별로 직렬화하며 서로 다른 lane은 동시에 실행할 수 있다. | 제공하지 않는다. |
| Relocation 경계 | Actor별 current turn 경계를 사용한다. | `SpotWide`는 기본적으로 임의의 안전한 turn 경계를 사용하고, 선택적으로 application이 알린 경계만 사용할 수 있다. `PerActor`는 Actor별 current turn 경계를 사용한다. | 현재 Spot turn 경계를 사용한다. |
| `Yield` | 지원하지 않는다. | `SpotWide`에서만 지원한다. `PerActor`에서는 지원하지 않는다. | 지원한다. |
| Logical Multicast subscription | 지원한다. | 지원한다. | 지원하지 않는다. |
| Application의 명시적 close | Entry Spot context와 manager에 close operation을 제공하지 않는다. | Exact `SpotRef`를 manager `Close`에 전달하거나 local context에서 close한다. | 자신의 handler나 timer context에서 close한다. |
| Relocation | Entry Spot 자체는 이동하지 않는다. Actor를 독립된 relocation unit으로 옮긴다. | `SpotWide`는 Spot과 member Actor 전체를 한 번에 옮긴다. `PerActor`는 Spot state를 옮기지 않고 Actor를 독립적으로 옮긴다. | Actor가 없는 Spot 하나를 relocation unit으로 이동한다. |
| Host shutdown | Accepted turn을 정리한 뒤 `HostShutdown` reason으로 `OnClosing`을 호출한다. | 같은 shutdown closing 계약을 적용한다. | 같은 shutdown closing 계약을 적용한다. |
| .NET 구현 type | `IZLinkEntrySpot`, Actor type을 지정하면 `IZLinkEntrySpot<TActor>` | `IZLinkSpot`, Actor type을 지정하면 `IZLinkSpot<TActor>` | `IZLinkInstanceSpot` |

Host relocation의 공통 단계와 Spot 종류별 sequence diagram은
[Graceful drain과 handoff §8](28-graceful-drain-handoff.ko.md#8-unit-하나를-이전하는-순서)이
정의한다.

Framework는 작업의 대상에 따라 실행을 기다릴 queue를 정한다. 세 종류의 Spot에
전달된 direct packet과 timer callback은
[Spot application queue](01-glossary.ko.md#spot-application-queue)에 넣는다.
Actor에 전달된 업무 payload는 Spot queue를 거치지 않고 해당 Actor의 queue에
바로 넣는다.

### 3.1 Relocation 중에는 temporary queue를 먼저 확인한다

일반 dispatch는 기존처럼 Ready Actor나 Spot의 execution queue를 찾아 message를 넣는다.
Target runtime이 Restore 요청을 받으면 다음 packet을 dispatch하기 전에
[relocation temporary queue](01-glossary.ko.md#relocation-temporary-queue)를 등록한다. 이후
Actor나 Spot message의 dispatch 순서는 다음과 같다.

1. Object 종류, ID와 `ObjectGeneration`을 검사한다.
2. 같은 `RelocationId`와 target attempt에 등록된 temporary queue가 있는지 확인한다.
3. 있으면 실제 application instance를 찾지 않고 temporary queue에 넣는다.
4. 없으면 기존 object lookup과 execution queue 경로를 사용한다.

Temporary queue에는 source ingress hold에서 relay한 message와 owner 변경 전후 target에
도착한 message가 함께 들어갈 수 있다. Target은 temporary queue의 application payload를
실행하지 않는다. Actor나 Spot 생성, state Restore, owner 변경과 필요한 lifecycle callback이
끝난 뒤 다음 순서로 실제 execution queue에 옮긴다.

```text
+----------------------------------------------------------------------+
| Target object queue                                                  |
|                                                                      |
| Restored work -> Temporary queue work -> New direct work             |
+----------------------------------------------------------------------+
```

이 전환은 dispatch와 atomic하게 처리한다. 전환 전에 temporary queue가 수락한 message를 실제
queue에 모두 넣은 뒤 temporary queue 등록을 제거한다. 동시에 들어온 message는 temporary
queue와 실제 queue 중 정확히 한 곳에만 들어간다. 실제 queue는 이 전환이 끝나기 전에
application handler를 실행하지 않는다.

저장했다가 복원한 기존 작업은 temporary queue의 message보다 먼저 처리한다. Temporary queue
안에서는 target dispatcher가 message를 수락한 순서를 유지한다. 서로 다른 network route에서
동시에 들어온 message 사이에 별도의 전역 순서를 만들지는 않는다.

`SpotWide` User Spot relocation에서는 Spot과 모든 member Actor를 같은 relocation group으로
등록한다. Temporary queue의 각 record는 실제 target Spot 또는 Actor identity를 보존한다.
복원이 끝나면 Spot message는 Spot queue, Actor message는 해당 Actor queue로 나눠 넣으며 각
target 안의 수신 순서를 유지한다. `PerActor`에서는 Spot과 Actor relocation을 독립적으로
등록하므로 이동하지 않는 Actor의 기존 dispatch를 막지 않는다.

같은 Restore 요청을 다시 받으면 기존 temporary queue와 Restore 진행 상태를 사용한다. 이전
target attempt나 다른 `ObjectGeneration`의 queue에는 message를 넣지 않는다. Commit 전 abort에서는
target temporary queue를 실행하지 않고 폐기하며 source가 보관한 작업을 원래 queue로 되돌린다.
Commit 뒤에는 같은 target process가 실행 중일 때만 temporary queue를 실제 queue로 옮긴다.
Target process가 종료되면 다른 runtime이 이 작업을 자동으로 이어받지 않는다.

Queue는 작업이 기다리는 위치를 정한다. Execution mode는 서로 다른 queue의 작업을
동시에 실행할 수 있는지 정한다. User Spot의 기본 `SpotWide` mode에서는 queue를
다음과 같이 사용한다.

```text
+----------------------------------------------------------------------+
| User Spot (SpotWide)                                                 |
|                                                                      |
| Direct packet ---+                                                   |
| Timer callback --+--> [Spot queue] -----------+                      |
|                                                |                     |
| Actor A payload -----> [Actor A queue] --------+                     |
|                                                +--> [SpotWide gate]  |
| Actor B payload -----> [Actor B queue] --------+          |          |
|                                                           v          |
|                                                    [One callback]    |
+----------------------------------------------------------------------+
```

이 그림에서 Spot queue와 Actor queue는 서로 분리되어 있다. Actor payload가
Spot queue를 경유하거나 여러 queue가 하나로 합쳐지는 것은 아니다. 다만 모든
queue가 하나의 공통 execution gate를 사용하므로, 같은 User Spot에서는 Spot
handler, timer callback과 member Actor handler 가운데 하나만 실행한다.

이 그림은 User Spot의 기본 `SpotWide` mode만 보여준다. Entry Spot은 Spot 작업과
Actor별 작업의 실행 범위를 분리한다. Instance Spot은 Actor membership을 지원하지
않으므로 Actor queue가 없다.

Entry Spot과 `PerActor` User Spot은 relocation에서도 같은 Actor 단위 모델을 사용한다.
Spot instance는 handler와 dependency를 제공하는 실행 shell이며 relocation 후 유지할
application state를 소유하지 않는다. Actor state, Actor queue와 Actor timer만 Actor
단위로 이전한다. 여러 Actor가 공유해야 하는 state는 application이 Redis, database
또는 별도 state service처럼 node 밖의 저장소에서 관리한다.

Spot handler는 Spot activation scope에서, Actor handler는 Actor activation scope에서
각각 한 번 만들어 재사용한다. Entry Spot과 `PerActor` User Spot의 서로 다른 Actor는
handler instance나 scoped dependency를 공유하지 않는다. 정확한 생성·정리와 relocation
규칙은 [Framework API의 handler 수명](06-framework-api.ko.md#82-handler-실행-객체와-dependency-수명)을
따른다.

`SpotWide` User Spot은 이 제한을 받지 않는다. Spot과 member Actor가 하나의
relocation aggregate이므로 Spot field와 Spot timer를 Spot relocation adapter로
함께 이전할 수 있다.

### 3.2 Spot 종류별 lifecycle callback

다음 표의 callback 이름은 .NET 표기를 사용한다. 다른 언어는 이름과 비동기 표현이
다를 수 있지만 호출 조건과 순서는 같다. `Configure`는 비동기 lifecycle callback이
아니라 handler를 등록하는 구성 단계이지만, Spot instance가 준비되는 순서를
함께 이해할 수 있도록 표에 포함했다.

| Callback | Entry Spot | User Spot | Instance Spot | 호출 목적 |
|---|---:|---:|---:|---|
| `Configure` | O | O | O | 해당 Spot instance가 사용할 handler를 등록한다. |
| `OnCreateAsync` | X | O | X | Manager가 새 User Spot을 만들 때 creation request를 확인하고 생성 수락 여부와 optional reply를 반환한다. 기존 User Spot을 찾은 `Existing` 결과에서는 호출하지 않는다. |
| `OnInitializeAsync` | O | O | O | 생성된 Spot instance의 application 초기화를 완료한다. Instance Spot은 `OnCreateAsync` 없이 이 callback을 사용한다. |
| `OnClosingAsync` | O | O | O | 아직 유효한 local Spot instance가 종료되기 전에 application resource를 정리한다. 호출 조건은 §3.4에서 구분한다. |
| `OnActorJoinAsync` | X | O¹ | X | 이미 존재하는 Actor가 User Spot으로 이동하려 할 때 target User Spot이 요청을 승인하거나 거부한다. Entry Spot 복귀는 기본 membership이므로 admission callback을 사용하지 않는다. |
| `OnJoinedActorAsync` | O¹ | O¹ | X | 일반 join의 membership commit이 끝났음을 target Spot에 알린다. Actor 최초 생성과 maintenance 복원에서는 호출하지 않는다. |
| `OnLeaveActorAsync` | O¹ | O¹ | X | Membership commit 뒤 Actor가 빠져나간 source Spot에 알린다. Actor 소멸을 뜻하지 않는다. |
| `OnDisconnectActorAsync` | O¹ | O¹ | X | 해당 Spot에 속한 Actor의 연결 단절을 알린다. |
| `OnCreateActorAsync` | O¹ | X | X | 새 Actor의 initial Entry Spot membership을 승인하거나 거절하고 optional reply를 반환한다. 일반 join callback과 구분한다. |

¹ Actor type을 지정해 Actor membership을 지원하는 Entry Spot 또는 User Spot에만
적용한다.

### 3.3 Actor membership callback은 source와 target에서 나누어 실행한다

Entry Spot과 User Spot은 서로 다른 Spot instance다. 두 종류가 같은 Actor membership
interface를 구현하더라도 callback은 이동 전 Spot과 이동 후 Spot에서 각각 실행한다.

Application이 User Spot으로 보내는 join에서는 target User Spot이
`OnActorJoinAsync`로 이동을 승인한다. Entry Spot 복귀는 별도 admission 없이
membership을 commit한다. 두 경우 모두 commit 뒤 target의 `OnJoinedActorAsync`와
source의 `OnLeaveActorAsync`를 실행한다. 따라서 User Spot에 있던 Actor가 Entry
Spot으로 돌아가더라도 Entry Spot의 `OnCreateActorAsync`와 `OnActorJoinAsync`를
호출하지 않는다. Entry Spot과 User Spot 사이의 양방향 callback 비교와 정확한
commit 순서는
[23 Spot과 Actor membership §4](15-spot-actor.ko.md#4-actor-join과-commit-순서)가
정의한다.

### 3.4 Spot instance가 종료될 때 호출하는 callback

`OnClosingAsync`는 Actor별 callback이 아니라 Entry·User·Instance Spot instance의
terminal lifecycle callback이다. Framework는 callback을 실행할 때 종료 이유와
absolute deadline을 전달한다.

| 종료 이유 | Entry Spot | User Spot | Instance Spot | 호출 조건 |
|---|---:|---:|---:|---|
| `ExplicitClose` | X | O | O | Application이 User·Instance Spot close를 시작하고 해당 local instance를 정상적으로 정리할 때 호출한다. |
| `HostShutdown` | O | O | O | Relocation 없이 host가 local Spot을 정리할 때 호출한다. |
| `RelocationOut` | X | O | O | User·Instance Spot owner를 target으로 commit한 뒤 source local instance를 정리할 때 호출한다. |
| `IdleEvicted` | X | X | O | Instance Spot이 유휴 기준을 넘겨 local instance를 내릴 때 호출한다. |

User Spot에 Actor membership이 남아 있어 explicit close가 `false`로 끝나면
`OnClosingAsync`를 호출하지 않는다. Standalone Actor만 다른 Entry Spot으로 이동하는
작업도 Entry Spot instance를 닫지 않으므로 Entry Spot의 `OnClosingAsync`를 호출하지
않는다. Host shutdown에서는 Actor membership과 local Spot instance가 아직 유효한
상태에서 callback을 실행하고, callback이 끝난 뒤 scope와 authority를 정리한다.

## 4. Entry Spot

### 4.1 Object Server의 Actor 진입점

Entry Spot은 Object Server role을 가진 MeshNode에 등록한다. Framework는 startup에서
Entry Spot ID를 발급하고 instance를 초기화한다. Initialization이 끝나기 전에는
descriptor와 resolver에 Entry Spot을 게시하지 않는다.

Entry Spot ID는 MeshNode의 diagnostic prefix와 Entry Spot 전용 marker를 사용한
`<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식이다. MeshNode와 Entry Spot은 각각 별도의 UUID v4를
생성하지만 두 UUID의 값 비교로 관계를 판정하지 않는다. 같은 MeshNode lifecycle에서는 RID를 유지하고
replacement lifecycle에서는 endpoint가 같아도 새 RID를 발급한다.

Location Store가 global Spot ID active conflict를 보고하면 새 UUID나 reservation을 만들지 않고 startup을
즉시 startup configuration error로 끝낸다. MeshNode descriptor는 lifecycle generation과 exact Entry Spot ID의
mapping을 게시한다. Actor placement와 Entry Spot join은 이 mapping을 사용하며 Spot ID 문자열을 parsing하지
않는다.

Actor를 새로 만들면 Framework가 선택한 owner MeshNode의 Entry Spot이 initial
membership을 처리한다. Actor 생성과 initial Entry Spot membership은 같은
[Ready](01-glossary.ko.md#ready) barrier 안에서 완료한다. Actor가 Entry Spot에
속하더라도 업무 message는 Entry Spot callback을 경유하지 않고 Actor queue로
전달한다.

### 4.2 Entry Spot의 Actor lifecycle

Actor type을 지정한 Entry Spot은 다음 세 상황을 구분한다.

| 상황 | Target Entry Spot | Source Spot |
|---|---|---|
| 새 Actor의 initial membership | `OnCreateActorAsync`로 승인·거절 → 승인 시 membership·Ready commit | 없음 |
| Application이 요청한 일반 `JoinEntrySpot` | Admission callback 없이 membership commit → `OnJoinedActorAsync` | Commit 뒤 source Entry Spot 또는 User Spot의 `OnLeaveActorAsync` |
| Host maintenance의 standalone Actor relocation | Application membership callback을 호출하지 않는다. | Application membership callback을 호출하지 않는다. |

`OnCreateActorAsync`는 새 Actor를 처음 Entry Spot에 배치할 때만 사용하며 생성 승인
여부와 optional reply를 반환한다. 거절하면 staging Actor와 reservation을 정리하고
Ready로 공개하지 않는다. 이미 존재하는 Actor가 User Spot에서 돌아오거나 다른 Entry
Spot에서 application join으로 이동하는 경우에는 `OnCreateActorAsync`와
`OnActorJoinAsync`를 호출하지 않는다.

Host `Relocate`가 standalone Actor를 다른 node의 Entry Spot으로 옮기는 작업은
application이 요청한 membership 변경이 아니다. Framework는 target에서 Actor
state를 복원하고 Actor owner와 target Entry Spot membership을 commit하지만 target의
`OnJoinedActorAsync`나 source의 `OnLeaveActorAsync`를 호출하지 않는다. Relocation
전용 application callback도 제공하지 않는다.

Target에서 Actor state와 queue를 복원하고 owner·membership을 commit하면 Target Actor가
message를 처리하기 시작한다. 이 Actor가 Session에 bind되어 있으면 target runtime은
`sessionActorLocationUpdateReqMsg`를 send하여 Session owner가 보관한 해당 Actor의 현재
전달 경로인 binding route를 target owner로 갱신한다. Route switch와 함께
bound-session accessor가 반환하는 current Actor location snapshot도 같은
ActorId·ObjectGeneration을 유지한 채 target MeshName·NodeRid로 갱신한다. 같은 Session에 bind된
relocation 대상에 포함되지 않은 다른 Actor의 route와 physical STREAM connection은 바꾸지 않는다.
Session owner는 갱신을 마치면 `sessionActorLocationUpdateResMsg`를 send한다. 응답이 없으면
target runtime은 최초 send 1초 뒤부터 1초, 2초, 4초, 5초 간격으로 같은 요청을 다시
보내고 이후에는 5초 간격을 유지한다. 응답을 기다리는 동안에도 Target Actor는 message를
처리하며 이전 route의 message는 Message Follow route가 전달한다. Route 갱신은 같은 `ObjectGeneration`에만 적용하며,
application은 relocation을 알기 위해 rebind하지 않는다. 새 incarnation은 application이
명시적으로 다시 bind해야 한다.

Application은 relocation 사실을 Entry Spot lifecycle callback으로 추적하지 않는다.

### 4.3 Entry Spot 자체는 이동하지 않는다

Entry Spot은 해당 Object Server lifecycle에 속하므로 relocation participant가
아니다. Host `Relocate`에서는 Entry Spot에 속한 Actor를 target node의 Entry Spot으로
옮기지만 source Entry Spot instance 자체를 옮기지는 않는다. Target Entry Spot은
target Object Server startup에서 Framework가 새 RID와 lifecycle로 준비한다.

Standalone Actor 이동은 Entry Spot을 닫는 작업이 아니므로 Entry Spot의
`OnClosing`을 호출하지 않는다. Host가 relocation 없이 shutdown될 때는 accepted
handler와 timer turn을 정리한 뒤 local Entry Spot에 `HostShutdown` closing
context를 전달한다.

## 5. User Spot

User Spot은 application이 stable type의 factory를 등록하고 manager를 사용해
명시적으로 만든다.

- `Create`는 caller가 stable type을 지정하고 Framework가 global Spot ID를 만든다.
- `GetOrCreate`는 caller가 global Spot ID와 stable type을 모두 지정한다.
- Actor membership을 지원하는 User Spot은 join·joined·leave·disconnect control을
  자신의 Spot queue에서 다른 callback과 직렬화한다.
- Current Actor membership이 하나라도 남아 있으면 public close는 `false`로 끝나며
  Framework가 member Actor를 숨겨서 이동하거나 제거하지 않는다.
- `SpotWide` relocation은 User Spot과 seal 시점의 member Actor를 하나의 aggregate로
  preflight하고 commit한다.
- `PerActor` relocation은 target에 stateless Spot shell을 준비하고 Spot authority를
  먼저 옮긴 뒤 member Actor를 독립된 unit으로 옮긴다.

User Spot의 기본 execution mode는 `SpotWide`다. 같은 User Spot의 Spot handler,
member Actor handler, timer와 lifecycle callback을 전체에서 한 번에 하나만 실행한다.
Factory 등록에서 `PerActor`를 선택하면 같은 Actor, 같은 Spot lane과 같은 timer만
각각 직렬화하고 서로 다른 lane은 동시에 실행할 수 있다. Execution mode는
MeshNode lifecycle을 시작하기 전에 고정하며 실행 중에는 바꾸지 않는다.

`Yield`는 `SpotWide`에서만 사용할 수 있다. Shared User Spot turn을 반납한 뒤
continuation은 같은 공통 gate를 다시 얻어 새 turn에서 재개한다. `PerActor`에는
shared Spot turn이 없으므로 `Yield`를 제공하지 않는다.

### 5.1 SpotWide relocation 경계

[`Spot relocation readiness mode`](01-glossary.ko.md#spot-relocation-readiness-mode)의
기본값은 `AnyTurnBoundary`다. 이 mode에서는 Framework가 현재 turn이 끝난 안전한
경계를 선택하므로 application이 별도 준비 신호를 보내지 않는다.

Round·match가 끝난 뒤에만 이동할 수 있는 Spot은 factory 등록에서
`ApplicationSignaled`를 선택한다. Application은 안전한 turn에서
`RelocationReady().Defer()`를 등록하고 handler를 끝낸다. `Defer()` 뒤 일반
Framework operation을 같은 turn에서 시작하면 `InvalidOperation`이다.

Framework는 등록한 경계 뒤 일반 application job을 잠시 보류하고 다음 중 하나를
처리한다.

| 조건 | 처리 owner | Completion outcome |
|---|---|---|
| 사용할 relocation이 없음 | 현재 owner | `Continued` |
| Relocation이 commit 전에 취소됨 | 복원한 source owner | `Continued` |
| Relocation이 완료됨 | target owner | `Relocated` |

Framework는 현재 owner에서 `OnRelocationReadyCompleted`를 다음 application job보다
먼저 호출한다. Callback이 완료되면 보류한 message와 timer를 다시 처리한다.
Application은 다음 round를 이 callback에서 시작할 수 있다.

Callback은 Spot interface의 기본 no-op 구현이다. `ApplicationSignaled`를 선택해도
override를 강제하지 않는다. 정상 실행에서는 readiness 등록마다 logical completion
하나를 만든다. Callback 실행 중 process가 종료되면 완료를 확인할 수 없으므로
recovery에서 같은 completion을 다시 호출할 수 있다. Override는 retry-safe해야 한다.

`AnyTurnBoundary`, `PerActor`, Entry Spot 또는 Instance Spot에서
`RelocationReady().Defer()`를 호출하면 queue mutation 전에
`InvalidOperation`으로 실패하고 completion callback을 호출하지 않는다.

Creation request, placement, `SpotRef`와 close의 exact generation 검사는
[24 Spot 주소 메시징](16-spot-address-messaging.ko.md)이 정의한다.

### 5.2 User Spot lifecycle

새 User Spot은 factory가 instance를 만든 뒤 `Configure`, `OnCreateAsync`와
`OnInitializeAsync`를 거쳐 Ready 상태가 된다. `OnCreateAsync`는 creation request를
검사하고 생성 수락 여부와 optional reply를 반환한다. 같은 stable type의 Ready User
Spot을 찾아 `Existing`으로 끝난 `GetOrCreate`에서는 factory와 `OnCreateAsync`를
실행하지 않는다.

Actor membership을 지원하는 User Spot은 일반 join에서 target이면
`OnActorJoinAsync`와 `OnJoinedActorAsync`를 실행하고, source이면 commit 뒤
`OnLeaveActorAsync`를 실행한다. Actor 연결 단절은 `OnDisconnectActorAsync`로
알린다. 이 callback들은 User Spot의 선택한 execution mode에 따라 Spot lifecycle
lane에서 실행한다.

`SpotWide` User Spot을 다른 node로 relocation할 때는 Spot과 member Actor의 logical
membership을 그대로 유지한다. 따라서 member Actor에 대해 Entry Spot 또는 User
Spot의 `OnActorJoinAsync`, `OnJoinedActorAsync`, `OnLeaveActorAsync`를 호출하지
않는다. Source User Spot instance를 정리할 때는 `RelocationOut` 이유로
`OnClosingAsync`를 호출한다.

Member Actor가 Session에 bind되어 있으면 Spot과 Actor를 target에 복원하고 aggregate owner를
commit한 뒤 target runtime이 각 Session owner에 `sessionActorLocationUpdateReqMsg`를 send한다.
Session owner는 aggregate에 포함된 각 Actor의 [binding route](01-glossary.ko.md#binding-route)를
target owner로 갱신한다. Route switch와 함께 각 bound-session accessor가 반환하는 current Actor
location snapshot도 같은 ActorId·ObjectGeneration을 유지한 채 target MeshName·NodeRid로 갱신한다.
같은 Session에 bind되어 있지만 이 aggregate에 포함되지 않은 Actor의 route와 physical STREAM
connection은 바꾸지 않는다. 각 Session owner는 갱신을 마치면
`sessionActorLocationUpdateResMsg`를 send한다. 응답이 없으면 target runtime은 정해진
1초, 1초, 2초, 4초, 이후 5초 간격으로 각 요청을 다시 보낸다. 응답을 기다리는 동안에도
target User Spot과 member Actor는 message를 처리한다. Route 갱신은 같은 `ObjectGeneration`에만 적용하며,
application은 relocation을 알기 위해 rebind하지 않는다. 새 incarnation은 application이 명시적으로
다시 bind해야 한다.

`PerActor` relocation에서는 target에 같은 `SpotId`와 `ObjectGeneration`을 사용하는
private Spot shell을 먼저 준비한다. 이 shell은 Location Store의 Spot authority가
target으로 바뀌기 전까지 application 요청을 받지 않는다. Authority가 바뀐 뒤 새
`ToSpot`, Actor Create와 Join은 target이 처리하고, source shell은 아직 source에
남은 Actor의 기존 작업과 relocation control만 처리한다.

Actor는 bounded concurrency로 각각 이전한다. Actor의 `ObjectGeneration`과 logical
User Spot membership은 유지하고 Actor owner generation만 바꾼다. Infrastructure
relocation은 `OnActorJoinAsync`, `OnJoinedActorAsync`, `OnLeaveActorAsync` 또는
`OnDisconnectActorAsync`를 호출하지 않는다. 마지막 Actor와 source에서 이미 수락한
Spot 작업을 모두 정리한 뒤 source shell에 `RelocationOut`을 전달하고 종료한다. 각 Actor가
Session에 bind되어 있으면 target runtime이 `sessionActorLocationUpdateReqMsg`를 send하여 해당
binding route와 bound-session current Actor location snapshot을 같은
ActorId·ObjectGeneration을 유지한 채 target owner 및 target MeshName·NodeRid로 갱신한다.
응답이 없으면 정해진 간격으로 재전송하며, 응답을 기다리는 동안에도 Target Actor는 message를
처리한다. Application은 이 갱신을 위해 rebind하지 않는다.

## 6. Instance Spot

Instance Spot은 Actor membership이 없는 Spot이다. Direct packet handler, timer와
outbound call은 사용할 수 있지만 다음 기능은 사용할 수 없다.

- Actor create·join·leave·relocation
- Logical Multicast subscription
- Manager `Create`·`GetOrCreate`

Spot direct call은 기본적으로 실행 중인 Spot만 찾는다. Missing RID에서 Instance
Spot을 준비하려면 같은 call에 Instance intent를 명시해야 한다. 일반 message와
`Find`는 hidden create를 시작하지 않는다. 최초 message를 보존하는 cold activation,
factory 실행과 Ready barrier는
[24 Spot 주소 메시징 §4](16-spot-address-messaging.ko.md#4-direct-message로-instance-spot-생성을-허용하는-방법)이
정의한다.

Instance Spot은 application handler나 timer가 자신의 context에서 close할 수 있다.
Host `Relocate`에서는 Actor가 없는 Spot 하나를 relocation unit으로 처리한다.
Instance Spot의 direct handler와 timer는 하나의 Spot execution gate를 사용한다.
`Yield`로 이 turn을 반납하면 다음 Instance Spot record를 실행할 수 있고,
continuation은 같은 gate에서 새 turn으로 재개한다.

### 6.1 Instance Spot lifecycle

Instance Spot은 Actor membership을 지원하지 않으므로 Actor
create·join·joined·leave·disconnect callback을 제공하지 않는다. Missing Instance
Spot의 cold activation에서는 factory가 instance를 만든 뒤 `Configure`와
`OnInitializeAsync`를 실행한다. User Spot 생성에 사용하는 `OnCreateAsync`나 빈
creation request를 사용하지 않으며, activation을 시작한 첫 업무 message를 Ready
전에 durable inbox의 첫 record로 보존한다.

Application이 자신의 context에서 정상적으로 닫으면 `ExplicitClose`, Host가
relocation 없이 종료하면 `HostShutdown`, relocation commit 뒤 source instance를
정리하면 `RelocationOut` 이유로 `OnClosingAsync`를 호출한다.

### 6.2 유휴 Instance Spot 정리

Framework는 Instance Spot을 유휴 기준으로 정리할 수 있다. **User Spot과 Entry Spot은
정리하지 않는다** — 일반 message는 없는 object를 만들지 않으므로([Spot·Actor
routing](18-object-routing.ko.md)) 정리한 User Spot으로 온 message는 되살아나지 못하고
실패한다. Instance Spot은 Instance intent를 명시한 call이 다시 cold activation하므로
정리해도 다음 call에서 복구된다.

정리는 다음 두 조건을 **함께** 만족할 때만 시작한다.

| 조건 | 내용 |
|---|---|
| 유휴 시간 | 마지막 application 작업 완료 이후 `InstanceSpotIdleTimeout`을 넘겼다. 기본값은 `0`이며 `0`은 정리하지 않음을 뜻한다. |
| 진행 중 작업 없음 | application queue와 timer queue가 비어 있고, 완료를 기다리는 operation과 relocation 참여가 없다. |

정리는 `IdleEvicted`로 `OnClosingAsync`를 호출한 뒤 local instance를 내리고 Location
Store의 owner record를 제거한다. Application 상태를 보존하지 않으므로, 유지해야 하는
상태는 `OnClosingAsync`에서 application이 저장한다.

정리 뒤 같은 ID로 온 Instance intent call은 새 `ObjectGeneration`으로 cold activation한다.
정리 뒤 도착한 일반 message는 `NotFound`로 끝난다.

## 7. .NET에서 보이는 차이

다음 코드는 Object Server builder에 선언된 세 registration method의 발췌다.
Entry Spot은 구현 type만 등록하지만 User·Instance Spot은 stable type, object 종류별
factory configure callback에서 option과 relocation policy를 함께 등록한다. Callback은 policy를 정확히
하나 선택해야 한다.

```csharp
IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
    where TEntrySpot : class, IZLinkEntrySpot;

IZLinkMeshObjectServerBuilder AddSpotFactory<TSpot>(
    string spotType,
    Action<IZLinkUserSpotFactoryBuilder<TSpot>> configure)
    where TSpot : class, IZLinkSpot;

IZLinkMeshObjectServerBuilder AddInstanceSpotFactory<TSpot>(
    string instanceSpotType,
    Action<IZLinkInstanceSpotFactoryBuilder<TSpot>> configure)
    where TSpot : class, IZLinkInstanceSpot;
```

세 Context는 공통 identity, outbound call, timer와 worker 기능을 공유한다. Framework는 factory를 호출하기
전에 `MeshName`, `SpotId`, `ObjectGeneration`, `NodeRid`와 owner fence가 결합된 exact Context를 만든다.
Factory가 반환한 User·Entry·Instance Spot은 전달받은 Context를 read-only member로 그대로 노출해야 하며,
다른 Context를 반환하면 staging Spot을 Ready로 공개하지 않는다. Same-node operation은 Spot instance와
Context를 유지한다. Cross-node relocation은 SpotId와 ObjectGeneration을 유지하고 target owner generation에
결합한 새 Context를 target factory에 전달하며 commit 뒤 source Context의 새 operation을 fence한다.
User Spot에는 Actor leave와 close가 있고 Instance Spot에는 close만 있다.

```csharp
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
    IZLinkSpotHandlerRegistry Handlers { get; } // Direct와 subscription handler

    ValueTask LeaveActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default);

    ValueTask<bool> CloseAsync(
        CancellationToken cancellationToken = default);
}

public interface IZLinkInstanceSpotContext : IZLinkSpotCommonContext
{
    IZLinkInstanceSpotHandlerRegistry Handlers { get; } // Direct handler만 등록

    ValueTask<bool> CloseAsync(
        CancellationToken cancellationToken = default);
}
```

Entry Spot은 close operation 대신 Actor destroy와 full Spot handler registry를
제공한다.

```csharp
public interface IZLinkEntrySpotContext : IZLinkSpotCommonContext
{
    IZLinkSpotHandlerRegistry Handlers { get; } // Direct와 subscription handler

    ValueTask DestroyActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default);
}
```

정확한 전체 interface와 lifecycle callback은
[.NET Spot interface](server/languages/dotnet/interfaces/05-spots.ko.md)가 소유한다.

## 8. 문서 경계

| 문서 | 소유하는 상세 계약 |
|---|---|
| [20 Spot 메시징](12-spot-messaging.ko.md) | Spot direct, Logical Multicast, queue admission과 dispatch |
| [21 MeshNode](13-mesh-node.ko.md) | Object role, Entry Spot과 factory 등록, placement capability |
| [23 Spot과 Actor membership](15-spot-actor.ko.md) | Actor 생성, Entry·User Spot membership과 callback·commit 순서 |
| [24 Spot 주소 메시징](16-spot-address-messaging.ko.md) | User·Instance Spot의 ID, 생성, cold activation, route와 close |
| [54 Host Relocate, Shutdown과 handoff](28-graceful-drain-handoff.ko.md) | 세 Spot 종류의 shutdown, relocation과 recovery 순서 |

## 9. 검증 요구

- Entry Spot은 Object Server startup에서 Framework가 Spot ID를 발급하고 initialization
  뒤에만 공개한다.
- Entry Spot ID가 MeshNode와 같은 diagnostic prefix, 별도로 생성한 UUID v4를 사용하고 descriptor가
  lifecycle generation과 exact RID의 mapping을 게시한다.
- Replacement lifecycle은 새 Entry Spot ID를 발급하고 active authority 충돌에서 즉시 실패한다.
- Caller가 예약된 Entry Spot 형식으로 User·Instance Spot ID를 지정하면 Store와 factory 실행 전에
  거부한다.
- User Spot manager만 명시적인 `Create`·`GetOrCreate`를 제공한다.
- Instance intent가 없는 일반 direct message와 `Find`는 Missing Instance Spot을
  만들지 않는다.
- Entry·User Spot은 Actor membership과 Logical Multicast subscription을 지원하고
  Instance Spot은 둘 다 거부한다.
- Actor 업무 payload는 Entry·User Spot callback을 경유하지 않고 Actor queue에
  직접 제출한다.
- Entry Spot 자체는 relocation하지 않으며 target Object Server startup에서 새
  identity로 준비한다.
- User Spot은 member Actor와 aggregate로 이동하고 Instance Spot은 Actor가 없는
  단일 relocation unit으로 이동한다.
