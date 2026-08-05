---
title: "Framework 메시징 용어집"
---

# Framework 메시징 용어집

[스펙 목차](README.ko.md) · [이전: Framework 공개 계약 관리](00-public-contract-governance.ko.md) · [다음: ZLink Framework 개요](02-overview.ko.md)

> **이 장이 정의하는 것** — 이 스펙 전체에서 쓰는 공통 domain term, 상태와 결과 이름.

[스펙 문서 작성 가이드](../../../../../doc/principal/documentation/spec-writing-guide.ko.md) ·
[Spot 메시징](12-spot-messaging.ko.md)

## 표와 .NET 코드 예제를 읽는 방법

값이나 record를 나타내는 용어에는 먼저 다음 요약 표를 둔다.

| 항목 | 의미 |
|---|---|
| 형태 | 단일 값, closed value, 복합 record, runtime 객체·상태·과정 중 무엇인지 표시한다. |
| .NET 표기 | 정식 .NET public type이 있으면 정확한 type을 표시한다. |
| 공개 구성 | 공개 계약이 정의한 field, 값 범위와 형식을 표시한다. |
| 생성·관리 | 값을 만들고 갱신하거나 폐기하는 주체를 표시한다. |
| 수명 | 값이 유효한 범위와 더 이상 사용할 수 없는 조건을 표시한다. |

`.NET 표기`에 **public type 없음**이라고 적은 값은 application에 독립된 type으로
노출되지 않는다. 이때 제시하는 C# 모양은 구조를 읽기 위한 **contract
pseudocode**이며 실제 API 이름이나 생성자를 뜻하지 않는다. 공개 계약이 opaque로
정의한 값에는 내부 field를 추측하여 추가하지 않는다.

실제 .NET public type이 있는 복합 값은 요약 표 아래에 정식 C# 선언을 보여준다.
선언과 member 이름은 exact interface를 그대로 사용하고, 각 member가 하는 일은
해당 줄의 한국어 주석으로 설명한다.

실제 .NET 선언의 단일 기준은
[.NET Server exact interface](server/languages/dotnet/interfaces/README.ko.md)와
[.NET Stream Connector exact interface](stream-connector/languages/dotnet/03-stream-connector.ko.md)다.
이 용어집의 .NET 표기는 공통 계약을 구체적으로 읽기 위한 보조 표기다.

## 1. Spot과 위치

<a id="spot"></a>
### Spot

주소와 상태를 가진 논리 instance다. Room, stage나 zone처럼 message를 받을 대상을
나타내며, 실제로 실행하는 node가 바뀌어도 같은 Spot ID로 접근할 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | Stateful runtime object |
| .NET 표기 | `IZLinkSpot`, `IZLinkInstanceSpot`, `IZLinkEntrySpot`; exact location snapshot은 `SpotRef` |
| 공개 구성 | Global Spot ID, Spot kind, stable type, ObjectGeneration, current owner와 실행 mode에 따른 application queue·execution gate를 가진다. |
| 생성·관리 | Framework Object runtime과 application Spot implementation이 함께 관리한다. |
| 수명 | 같은 incarnation은 ObjectGeneration으로 구분하며 owner가 이동해도 global Spot ID는 유지된다. |

<a id="spot-id"></a>
### Spot ID

Spot을 식별하는 전역 논리 주소다. 같은 Location Store 범위에서 중복할 수 없으며
Framework가 현재 Spot이 어느 node에 있는지 찾을 때 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | UTF-8 string |
| .NET 표기 | `string` |
| 공개 구성 | UTF-8 encoded 크기 1..255 bytes의 case-sensitive exact string이다. MeshName, Spot kind와 stable type은 ID에 포함되지 않는다. |
| 생성·관리 | Entry Spot은 Framework가 발급하고 User·Instance Spot은 application이 manager call에 지정한다. |
| 수명 | Location Store namespace 전체에서 Spot identity로 유지된다. 같은 값을 다른 Spot kind나 stable type에 다시 사용할 수 없다. |

특정 Spot incarnation과 현재 위치를 함께 가리킬 때는 다음 `SpotRef`를 사용한다.

```csharp
public readonly record struct SpotRef(
    string SpotId,           // Spot의 전역 논리 주소
    ulong ObjectGeneration,  // 같은 ID로 다시 생성된 Spot을 구분하는 incarnation
    string MeshName,         // 이 exact incarnation이 속한 Mesh
    RoutingId NodeRid);      // 이 snapshot을 만들 때의 owner node
```

`SpotRef`는 immutable location snapshot이며 local Spot instance나 runtime resource를
소유하지 않는다. 일반 Spot message는 Spot ID만 받고 current authority를 다시
찾는다.

<a id="entry-user-instance-spot"></a>
### Entry Spot, User Spot과 Instance Spot

- Entry Spot은 Framework가 Spot ID를 발급하고 server 진입점으로 제공한다.
- User Spot은 application이 명시적으로 생성하고 관리하는 Spot이다.
- Instance Spot은 별도 create call 없이 최초 message로 필요한 시점에 준비할 수
  있는 Spot이다.

| 종류 | .NET public type | 생성 방식 |
|---|---|---|
| Entry Spot | `IZLinkEntrySpot` | Object Server startup에서 등록하고 Framework가 Spot ID를 발급한다. |
| User Spot | `IZLinkSpot` | Application이 `IZLinkSpotManager.Create` 또는 `GetOrCreate`로 명시적으로 만든다. |
| Instance Spot | `IZLinkInstanceSpot` | `IZLinkSpotSendCall`·`IZLinkSpotRequestCall`의 `InstanceSpot(...)` intent가 있을 때 최초 message로 준비한다. |

Entry Spot ID는 `<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식으로 Object Server MeshNode
lifecycle마다 발급한다. MeshNode와 Entry Spot은 같은 prefix를 사용하되 각각 별도의 UUID v4를 생성한다.
Descriptor가 MeshNode와 exact Entry Spot ID의 관계를 기록하며 application은 Spot ID 문자열을 parsing해 node
관계를 추론하지 않는다. 같은 lifecycle에서는 RID를 유지하고 replacement lifecycle에서는 새 RID를
발급한다. Global Spot ID authority가 충돌하면 새 UUID나 reservation을 만들지 않고 startup을 즉시
configuration error로 끝낸다. 이 형식은 Framework 발급용으로 예약하므로 caller가 같은 형식의
User·Instance Spot ID를 지정하면 Store와 factory를 실행하기 전에 `InvalidOperation`으로 거부한다.

세 종류의 기능, Actor membership, close와 relocation 차이는
[Spot 모델](11-spot-model.ko.md)이 정의한다.

<a id="actor-membership"></a>
### Actor membership

Actor가 현재 어느 Entry Spot 또는 User Spot에 속하는지 나타내는 관계다. Location
Store가 이 관계의 기준을 보관한다. Actor를 다른 Spot이나 node로 옮길 때는 Actor
owner와 source·target Spot의 membership을 한 번의 Location Store 변경으로 함께
바꾼다.

Channel이나 Mesh에 node가 참여한다는
[Membership](#membership)과는 다른 개념이다.

<a id="user-spot-execution-mode"></a>
### User Spot execution mode

User Spot 안에서 Spot handler, member Actor handler와 timer callback이 어느 execution gate를 공유할지
정하는 startup 등록 옵션이다.

| Mode | 실행 단위 | 동시에 실행할 수 있는 범위 | `Yield` |
|---|---|---|---|
| `SpotWide` | User Spot 전체가 shared execution gate 하나를 사용한다. | 같은 User Spot의 Spot handler, member Actor handler, timer와 lifecycle callback을 한 번에 하나만 실행한다. Relocation은 Spot과 member Actor 전체를 하나의 aggregate로 옮긴다. | Shared turn을 반환할 수 있다. |
| `PerActor` | Spot lane, Actor별 lane과 timer별 lane을 분리한다. | 서로 다른 lane은 동시에 실행할 수 있다. 같은 Actor와 같은 timer 안에서는 순서를 유지한다. Relocation은 Spot state를 옮기지 않고 Actor를 독립적으로 옮긴다. | Shared Spot turn이 없으므로 사용할 수 없다. |

`SpotWide`가 기본값이다. Mode는 User Spot stable type을 등록할 때 고정하며 같은 MeshNode lifecycle 도중
변경하지 않는다. Entry Spot과 Instance Spot에는 이 옵션을 적용하지 않는다.

`PerActor` User Spot의 Spot instance는 handler와 dependency를 제공하지만 relocation
후 유지할 application state를 소유하지 않는다. 유지해야 하는 shared state와
Spot-level schedule은 application이 node 밖의 저장소에서 관리한다. Target에서는
같은 SpotId와 ObjectGeneration으로 Spot instance를 다시 만들고 Actor state,
Actor queue와 Actor timer만 Actor별로 이전한다.

<a id="spot-relocation-readiness-mode"></a>
### Spot relocation readiness mode

`SpotWide` User Spot이 어느 turn 경계에서 relocation을 시작할 수 있는지 정하는
startup 등록 옵션이다.

| Mode | 의미 |
|---|---|
| `AnyTurnBoundary` | Framework가 일반적인 안전한 turn 경계를 선택한다. 기본값이다. |
| `ApplicationSignaled` | Application이 현재 turn 뒤가 안전하다고 `RelocationReady().Defer()`로 알린 경계만 사용한다. |

`ApplicationSignaled`에서 `Defer()`는 relocation을 요청하지 않는다. 현재 host에
준비된 relocation이 있으면 그 경계를 사용하고, 없으면 같은 owner에서 계속한다.
두 경우 모두 Framework는 다음 application job보다 먼저
`OnRelocationReadyCompleted` callback을 호출한다.

Callback은 언어별 Spot interface에 기본 no-op 구현으로 제공한다. Application은
round나 match의 다음 단계를 callback에서 시작해야 할 때만 구현한다.
`AnyTurnBoundary`, `PerActor`, Entry Spot과 Instance Spot에서 `Defer()`를 호출하면
queue를 바꾸기 전에 `InvalidOperation`으로 실패한다.

<a id="meshnode"></a>
### MeshNode

RouteMesh에 참여하여 message를 보내거나 받는 runtime node다. Object Server role을
가진 MeshNode는 Spot factory와 lifecycle을 제공할 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | RouteMesh runtime component |
| .NET 표기 | Startup 구성은 `IZLinkMeshNodeBuilder`, 실행 상태 관측은 `IZLinkRouteMeshRuntime`과 `ZLinkMeshNodeSnapshot` |
| 공개 구성 | MeshName, Routing ID, ROUTER listener, peer set, Channel membership와 optional Object role을 가진다. |
| 수명 | Host가 해당 RouteMesh component를 시작한 때부터 drain·shutdown 완료까지 유지된다. |

<a id="routemesh"></a>
### RouteMesh

여러 MeshNode가 참여하여 node와 Channel message를 주고받는 범위다. ChannelName은
특정 RouteMesh에 참여한 node 중에서 message를 받을 후보를 정하는 데 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | MeshName으로 구분하는 distributed topology |
| .NET 표기 | `IZLinkFrameworkOptions.AddRouteMesh(string)`이 `IZLinkMeshNodeBuilder`를 반환한다. |
| 공개 구성 | 같은 MeshName의 MeshNode와 그 peer connection, Routing ID namespace와 Channel membership으로 구성된다. |
| 수명 | 참여 MeshNode별 lifecycle을 가지며 Framework가 서로 다른 MeshName 사이를 자동 relay하지 않는다. |

<a id="location-store"></a>
### Location Store

각 Spot의 현재 owner, ObjectGeneration과 lifecycle 상태를 여러 node가 함께 확인할
수 있도록 보관하는 저장소다. Spot을 새로 생성할 target이 하나로 결정되도록 생성
권한도 조정한다.

| 항목 | 내용 |
|---|---|
| 형태 | Distributed provider capability |
| .NET 표기 | `IZLinkLocationStore`; descriptor, owner lease와 authority transaction을 하나의 provider interface로 제공한다. |
| 공개 구성 | Descriptor, host owner lease, Spot·Actor location, durable authority, placement reservation과 generation counter를 관리한다. |
| 수명 | Host마다 하나의 provider instance를 등록한다. Ephemeral descriptor와 durable authority는 서로 다른 수명 규칙을 사용한다. |

<a id="object-role"></a>
### Object Client와 Object Server role

- Object Client는 Spot 생성, 조회와 messaging을 요청할 수 있지만 Spot factory를
  제공하지 않는다.
- Object Server는 Client 기능을 포함하며 Spot factory, Entry Spot과 lifecycle을
  제공할 수 있다.

Object Client는 object 기능에서만 outbound-only다. Spot·Actor factory와 application
Node direct handler는 제공하지 않지만, 독립된 RouteMesh Channel Server는 같은
MeshNode에 등록할 수 있다. 두 node가 모두 Object Client이고 양쪽 모두 RouteMesh
Channel Server membership이 없을 때만 peer connection을 생략한다. 어느 한쪽에
Server membership이 있으면 Channel weight가 `0`이어도 연결이 필요하다.

| 항목 | 내용 |
|---|---|
| 형태 | Closed MeshNode Object role |
| .NET 표기 | 구성은 `IZLinkMeshObjectRoleBuilder.Client()`·`Server()`, 관측은 `ZLinkMeshNodeObjectRole` |
| 공개 구성 | `None`, `Client`, `Server`이며 Server는 Client 기능과 factory·lifecycle 제공 기능을 포함한다. |
| 수명 | MeshNode startup configuration에서 고정한다. |

<a id="meshname"></a>
### MeshName

하나의 RouteMesh 물리 연결 그룹을 식별하는 이름이다. 같은 MeshName으로 등록한
MeshNode가 같은 RouteMesh에 참여한다. Object를 처음 배치할 때는 어느 RouteMesh의
node를 후보로 사용할지도 이 이름으로 지정한다. Spot ID의 일부는 아니며 이미
생성된 Spot의 current owner를 다시 정하는 값으로 사용하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | 논리 namespace 이름 |
| .NET 표기 | `string` |
| 공개 구성 | 문자열 하나다. Routing ID나 endpoint를 포함하지 않는다. |
| 생성·관리 | Application이 RouteMesh 등록과 optional 최초 Mesh 선택에 지정한다. |
| 수명 | RouteMesh registration 동안 같은 topology 이름으로 유지된다. 최초 placement에 사용한 뒤에도 Spot identity나 current owner의 일부가 되지 않는다. |

<a id="spot-kind"></a>
### Spot kind

Entry, User와 Instance 중 어떤 종류의 Spot인지 나타내는 값이다. 같은 Spot ID를
다른 kind로 다시 사용할 수 없다.

| 항목 | 내용 |
|---|---|
| 형태 | Closed enum |
| .NET 표기 | `ZLinkSpotKind` |
| 공개 구성 | `Invalid = 0`, `Entry = 1`, `User = 2`, `Instance = 3` |
| 생성·관리 | Framework가 Spot 등록·생성 방식에 따라 확정한다. |
| 수명 | 같은 Spot ID의 lifecycle 동안 바뀌지 않는다. |

<a id="stable-type"></a>
### Stable type

배포 version이나 실행 node가 바뀌어도 같은 종류의 Spot임을 식별하는 고정 이름이다.
Instance Spot을 새로 준비할 때 어떤 factory를 사용할지 결정하는 데 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Case-sensitive stable name |
| .NET 표기 | `string` |
| 공개 구성 | UTF-8 1~255 byte 문자열 하나다. Unicode normalization과 case folding을 적용하지 않는다. |
| 생성·관리 | Application이 factory 등록 시 지정한다. 언어 class FQN을 자동으로 사용하지 않는다. |
| 수명 | Store와 wire에서 Spot type identity로 유지된다. 같은 Object Server에 중복 등록할 수 없다. |

<a id="objectgeneration"></a>
### ObjectGeneration

같은 ActorId 또는 Spot ID의 서로 다른 logical incarnation을 구분하는 번호다. 이전
generation의 lifecycle·relocation control이 새 incarnation의 상태를 바꾸지 않도록
사용한다. 일반 Actor·Spot message는 logical ID가 가리키는 current Ready object를
대상으로 하므로 `ObjectGeneration`을 target 일치 조건으로 사용하지 않는다. Relocation 중 target에서
`RecreateOnRelocation` policy로 application 객체를 다시 만들어도 같은 logical incarnation을 계속
실행하므로 이 값은 유지한다.

| 항목 | 내용 |
|---|---|
| 형태 | 증가하는 generation 값 |
| .NET 표기 | `ulong` |
| 공개 구성 | `1..long.MaxValue` 범위의 정수 하나다. JSON에서는 decimal string으로 표현한다. |
| 생성·관리 | Location Store provider의 transaction domain global counter가 발급한다. |
| 수명 | 같은 logical incarnation 동안 유지된다. Same-node Join, cross-node relocation과 `RecreateOnRelocation`에서도 바꾸지 않는다. Logical incarnation을 종료한 뒤 새 object를 만들 때 새 값을 발급한다. 최대값 뒤에는 wrap하지 않고 `GenerationExhausted`로 실패한다. |

<a id="owner"></a>
### Owner

현재 Actor나 Spot을 실제로 실행하고 그 application queue를 관리하는 MeshNode다.
Application은 owner를 직접 지정하지 않고 Framework가 Location Store를 통해 찾는다.

| 항목 | 내용 |
|---|---|
| 형태 | Authority가 가리키는 current MeshNode role |
| .NET 표기 | 독립 application type 없음. Provider 계약에서는 `OwnerId`, `ZLinkLocationOwnerToken`과 owner node descriptor로 표현한다. |
| 공개 구성 | Owner identity, owner lease generation, MeshNode RID와 lifecycle generation을 authority와 함께 검증한다. |
| 수명 | Authority owner transition까지 current다. Relocation이나 takeover가 성공하면 새 owner generation으로 교체된다. |

<a id="authority"></a>
### Authority

Actor나 Spot이 현재 어느 node에 존재하며, 어느 node가 현재 owner인지 판단하는
기준 정보다. Location Store가 이 정보를 관리하므로 여러 node가 서로 다른 owner를
현재 owner라고 판단할 수 없다.

Authority는 단순한 endpoint나 송신 경로가 아니다. Object identity와 membership,
`ObjectGeneration`, `AuthorityOwnerGeneration`, `StoreVersion`과 exact owner lease를
함께 기록한다. Framework는 이 값들을 확인하여 다음을 구분한다.

- 같은 Spot ID로 다시 만들어진 새 object와 이전 object
- 현재 owner와 이전 owner
- 현재 Location Store 기록과 변경되기 전의 오래된 기록

따라서 이전 owner가 늦게 보낸 control이나 이전 object generation을 대상으로 한
lifecycle 변경을 현재 object에 적용하지 않는다. 일반 Actor·Spot message는 authority의
logical ID와 current Ready owner를 사용하며 `ObjectGeneration`으로 handler target을 제한하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | 복합 durable record |
| .NET 표기 | `ZLinkAuthorityKey`와 `ZLinkAuthoritySnapshot` |
| 공개 구성 | Object identity, current owner와 owner lease, lifecycle state, `ObjectGeneration`, `AuthorityOwnerGeneration`, `StoreVersion`, membership과 placement allocation을 포함한다. |
| 생성·관리 | Location Store provider가 reservation과 compare-exchange transaction으로 생성·변경한다. |
| 수명 | Explicit fenced delete까지 유지하며 TTL로 삭제하지 않는다. |

Authority record를 조회할 때는 object의 전역 논리 key를 다음 타입으로 전달한다.

```csharp
public readonly record struct ZLinkAuthorityKey(
    string Value); // Object kind와 global logical key에 대응하는 provider key
```

### Compare-and-set

Store에서 값을 읽을 때 받은 version이 그대로일 때만 값을 바꾸는 조건부 변경이다.
다른 요청이 먼저 값을 바꿨으면 변경하지 않고 충돌을 반환한다. Framework는 이
방식으로 같은 Actor·Spot의 owner나 membership을 동시에 두 요청이 서로 다르게
바꾸지 못하게 한다. 문서에서는 줄여서 CAS라고 쓴다.

CAS가 여러 record를 대상으로 할 때는 조건 확인과 모든 변경을 한 Store 요청에서
처리한다. 하나라도 조건이 다르면 어떤 record도 변경하지 않는다.

`ZLinkAuthoritySnapshot`의 공개 field는 다음과 같다.

```csharp
public sealed record ZLinkAuthoritySnapshot(
    string StoreVersion,                         // 현재 authority revision을 비교하는 version
    ReadOnlyMemory<byte> Payload,                // Framework가 encode한 opaque lifecycle payload
    ulong ObjectGeneration,                     // 같은 key의 object incarnation
    ulong AuthorityOwnerGeneration,             // 같은 incarnation에서 owner가 바뀐 순서
    string OwnerId,                              // Current owner identity
    long OwnerLeaseGeneration,                  // Current owner process lifecycle fence
    ZLinkPlacementAllocation Allocation,        // Pending 또는 Active capacity allocation
    ZLinkPendingObjectCreation? PendingCreation, // Creating 상태에서만 존재하는 creation 정보
    DateTimeOffset StoreNow);                    // Provider가 반환한 store 기준 시각
```

<a id="ready"></a>
### Ready

Spot 생성과 초기화, Location Store 기록이 끝나 application message를 받을 수 있는
상태다. Spot direct call은 일반적으로 Ready Spot의 owner에게 message를 보낸다.

| 항목 | 내용 |
|---|---|
| 형태 | Lifecycle state |
| .NET 표기 | 기능별 state enum·snapshot의 `Ready` 값으로 표현하며 공통 단일 `Ready` type은 없다. |
| 공개 구성 | Listener·transport admission 또는 object 생성·초기화처럼 기능별 serving 조건이 모두 끝난 상태다. |
| 수명 | Drain, disconnect, relocation, close나 fencing이 시작되면 새 admission의 Ready 상태에서 제외된다. |

<a id="admission-seal"></a>
### Admission seal

Framework가 정한 범위에서 새 application 작업을 더 이상 받지 않도록 전환하는
동작이다. 이미 수락한 handler, reply와 복구 작업은 해당 operation의 deadline까지
계속 처리할 수 있다.

Host shutdown에서는 host 전체에 적용한다. Actor·Spot relocation에서는 이동 대상
하나의 message, timer와 아직 시작하지 않은 continuation에 적용한다. Admission
seal은 이미 실행 중인 callback을 강제로 취소한다는 뜻이 아니다.

<a id="owner-route"></a>
### Owner route

Source runtime에서 current owner까지 message를 전달하는 송신 경로다. Owner가
바뀌면 Framework는 새 owner route를 다시 찾는다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework-managed routing state |
| .NET 표기 | Public route type 없음 |
| 공개 구성 | Current owner MeshNode identity, transport route와 object generation fence를 결합한다. |
| 수명 | Current authority와 transport readiness가 유지되는 동안만 사용할 수 있다. |

<a id="owner-fence"></a>
### Owner fence

현재 owner의 작업과 이전 owner가 늦게 보낸 작업을 구분하는 값이다. 수신 node는
이 값이 현재 owner와 맞지 않으면 message를 Spot queue에 넣지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | 여러 generation과 owner token을 함께 검사하는 fence |
| .NET 표기 | 독립 public type 없음 |
| 공개 구성 | 해당 operation의 `ObjectGeneration`, `AuthorityOwnerGeneration`, `OwnerId`와 `OwnerLeaseGeneration`을 current authority와 exact 비교한다. Operation에 따라 expected `StoreVersion`도 함께 검사한다. |
| 생성·관리 | Framework가 authority를 읽거나 reservation을 받을 때 값을 고정한다. |
| 수명 | Authority owner나 owner process lifecycle이 바뀌면 이전 fence는 stale이 된다. |

<a id="target-descriptor-fence"></a>
### Target descriptor fence

Source가 target을 선택할 때 확인한 target 등록 정보의 version이다. Target의 role,
등록 type이나 lifecycle 정보가 선택 후 바뀌었는지 판별하는 데 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Target descriptor identity와 lifecycle을 고정한 복합 fence |
| .NET 표기 | `ZLinkMeshNodeDescriptorKey`, `ulong` lifecycle generation과 `ZLinkLocationOwnerToken`의 조합 |
| 공개 구성 | MeshName·RID descriptor key, target lifecycle generation과 exact owner lease token을 포함한다. Reservation에서는 capacity delta와 descriptor 조건도 함께 검증한다. |
| 생성·관리 | Source가 target을 선택할 때 고정하고 target과 Store가 reservation 전에 다시 확인한다. |
| 수명 | Descriptor lifecycle이나 owner lease가 바뀌면 stale이 된다. |

<a id="positive-route-cache"></a>
### Positive route cache

최근에 확인한 Ready Spot의 owner route를 source runtime에 잠시 보관한 정보다.
사용할 수 있는 cache가 없거나 오래되었으면 Location Store에서 다시 조회한다.

| 항목 | 내용 |
|---|---|
| 형태 | Source runtime 내부 cache entry |
| .NET 표기 | Public type 없음 |
| 공개 구성 | Ready object key, current owner route와 admission에 필요한 generation fence를 보관한다. 내부 storage 형식은 공개 계약이 아니다. |
| 생성·관리 | Framework가 성공한 Ready authority 조회 결과로 만들고 source runtime에서 관리한다. |
| 수명 | `RouteCacheMaxAge`, owner admission deadline과 [Message Follow duration](#message-follow-duration)의 제한을 넘지 않는다. Missing, Creating과 Store failure는 positive cache로 저장하지 않는다. |

<a id="creation-attempt"></a>
### Creation attempt

하나의 logical object를 만들기 위해 예약부터 최종 결과 기록까지 수행하는 한 번의
생성 시도다. 같은 object에 여러 caller가 동시에 `GetOrCreate`를 호출하면 Location
Store reservation이 factory와 callback 실행을 하나씩 직렬화한다. 다른 operation은
먼저 시작한 시도의 application 결과를 공유하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Location Store가 관리하는 durable state |
| 식별 값 | [Reservation ID](#reservation-id) |
| 시작 상태 | `Reserved` |
| 최종 상태 | Authority는 Ready commit 또는 Creating cleanup으로 끝난다. Operation 결과는 별도 terminal record에 `Created`, `Rejected`, `Failed`로 기록한다. |
| 생성 실행 | Reservation CAS에 성공한 caller만 factory와 생성 callback을 실행한다. |
| 수명 | Reservation은 Ready commit 또는 Creating cleanup까지 유지한다. Operation terminal은 original deadline 뒤 5분까지 유지한다. |

<a id="reservation-id"></a>
### Reservation ID

Location Store에서 생성 또는 relocation을 위해 확보한 수용 공간과 진행 record를
구분하는 식별자다. 생성용 ID와 relocation용 ID는 서로 다른 namespace를 사용한다.
같은 ID와 같은 요청을 다시 보내면 앞서 발급한 결과를 반환한다. 같은 ID로 내용이
다른 요청을 보내면 `Conflict`다.

Creation에서는 process 재시작 뒤 같은 작업을 계속하거나 정확히 그 작업만 취소할 때
사용할 수 있다. relocation에서는 실행 중인 source와 target process 안에서
중복 요청을 구분하는 데만 사용하며 process 종료 뒤 작업을 이어받지 않는다. 서로
다른 operation을 같은 application 결과에 합류시키는 식별자가 아니다.

```csharp
public readonly record struct ZLinkCreationReservationId(
    string Value); // 한 creation attempt를 식별하는 opaque value
```

위 코드는 문서에서 구조를 설명하기 위한 표기다. 실제 public type의 이름과
encoding은 언어별 interface 계약을 따른다.

<a id="creation-terminal-result"></a>
### Creation terminal result

Creation attempt가 더 진행되지 않음을 나타내는 최종 결과다.

| 상태 | 의미 | Ready authority와 capacity |
|---|---|---|
| `Created` | Application이 생성을 승인하여 object가 Ready가 되었다. | Ready authority를 만들고 reserved capacity를 active capacity로 전환한다. |
| `Rejected` | Application callback이 정상적으로 실행되었지만 생성을 거절했다. Optional application reply를 포함할 수 있다. | Ready authority와 active capacity를 만들지 않고 reserved capacity를 반환한다. |
| `Aborted` | Node 종료, timeout 또는 callback exception 때문에 정상적인 승인·거절 결과를 만들지 못했다. Typed creation failure를 포함한다. | Ready authority와 active capacity를 만들지 않고 reserved capacity를 반환한다. |

`Existing`은 creation terminal result가 아니다. 이미 Ready인 object를 조회한 결과이므로
새 creation attempt, reservation 또는 생성 callback 실행이 없다.

## 2. Instance Spot 준비

<a id="instance-intent"></a>
### Instance intent

Target Spot이 없을 때 새 Instance Spot을 준비해도 된다는 caller의 명시적 선택이다.
.NET에서는 Spot direct call에 `InstanceSpot(...)`을 지정하여 표현한다.

| 항목 | 내용 |
|---|---|
| 형태 | Fluent call option |
| .NET 표기 | `IZLinkSpotSendCall.InstanceSpot(...)`, `IZLinkSpotRequestCall.InstanceSpot(...)` |
| 공개 구성 | Instance activation 허용 여부와 optional stable type으로 표현한다. |
| 수명 | 해당 single-use call에만 적용하며 일반 Spot direct나 이후 call에 남지 않는다. |

<a id="cold-activation"></a>
### Cold activation

Location Store의 authority가 `Missing`이고 caller가 Instance intent를 지정했을 때
새 Instance Spot을 만들고 초기화하여 최초 message를 받을 수 있게 준비하는
과정이다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework lifecycle process |
| .NET 표기 | 독립 public type 없음. `InstanceSpot(...)` call과 `IZLinkInstanceSpot` lifecycle callback으로 관찰한다. |
| 공개 구성 | Target 선택, durable envelope 저장, reservation, factory·initialize, inbox first record, Ready commit과 first handler dispatch 단계로 구성된다. |
| 수명 | Missing authority에서 시작하여 Ready 또는 fenced terminal failure로 끝난다. |

<a id="activation-envelope"></a>
### Activation envelope

최초 application message와 Spot 생성 및 reply에 필요한 정보를 함께 담아 target에
보내는 전달 단위다. Target은 이 message를 보관했다가 Spot이 Ready가 된 뒤 같은
Spot queue에 한 번 넣는다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework 내부 복합 envelope |
| .NET 표기 | public type 없음. 아래 구성 표는 contract pseudocode다. |
| 생성·관리 | Source Framework가 Missing Instance Spot call을 시작할 때 한 번 만들고 target runtime과 Relocation Store가 보존한다. |
| 수명 | First handler terminal completion과 replay cursor 갱신 뒤 recovery pointer가 해제될 때까지 유지한다. |

| 공개 구성 | 의미 |
|---|---|
| 최초 application message | Ready 뒤 같은 Spot queue에서 처리할 payload |
| Send/request kind | One-way인지 reply가 필요한 request인지 구분 |
| Operation identity | Retry와 중복 envelope가 같은 작업인지 구분 |
| Reply correlation | Request와 terminal reply를 연결 |
| Deadline | 전체 activation과 request에 적용할 마지막 시점 |
| Source identity | Source node RID, lifecycle generation과 optional source Spot ID |
| Target identity | Global Spot ID, 선택한 MeshName·stable type과 target descriptor fence |
| Metadata | Command 39의 optional metadata 존재 여부와 immutable metadata frame |

<a id="operation-identity"></a>
### Operation identity

Retry나 중복 전달이 같은 작업에서 나온 것인지 구분하는 값이다. 같은 최초 message를
두 번 처리하지 않도록 판단할 때 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Opaque operation identifier |
| .NET 표기 | Public type 없음 |
| 공개 구성 | 단일 opaque 값이다. 길이와 내부 encoding은 공개 계약이 아니다. |
| 생성·관리 | Framework가 terminal-once operation을 시작할 때 생성하고 redirect·recovery에서도 같은 값을 유지한다. |
| 수명 | 해당 operation의 terminal completion과 중복 판정에 필요한 기간 동안 유효하다. Application이 생성하거나 해석하지 않는다. |

<a id="actor-join-operation-id"></a>
### Actor Join OperationId

Actor Join completion callback이 같은 결과를 다시 전달한 것인지 application이
구분할 수 있게 하는 0이 아닌 128-bit 값이다. Relocation 실행 자체를 식별하는
`RelocationId`, placement reservation ID와 bounded aggregate commit ID는 각각
다른 목적의 내부 ID이며 이 값을 대신 사용하지 않는다.

```csharp
public readonly record struct ZLinkActorJoinOperationId(
    ulong High, // 128-bit ID의 상위 64 bits
    ulong Low); // 128-bit ID의 하위 64 bits
```

| 항목 | 내용 |
|---|---|
| 형태 | 두 개의 `ulong`으로 구성한 non-zero 128-bit value |
| .NET 표기 | `ZLinkActorJoinOperationId` |
| 공개 구성 | `High`와 `Low`를 함께 비교해야 한다. Application이 각 field에 별도 의미를 부여하지 않는다. |
| 생성·관리 | Framework가 Actor Join registration에서 생성하고 모든 completion retry에 같은 값을 전달한다. |
| 전달 | `Accepted`, `Rejected`, `Failed` Actor Join completion에 포함한다. Cross-node `Accepted`에서는 Relocation manifest의 별도 field에도 저장한다. |
| 수명 | Same-node outcome, `Rejected`와 commit 전 `Failed`는 current process lifetime까지만 retry를 보장한다. Cross-node `Accepted`는 manifest가 유지되는 동안 durable at-least-once completion에 사용한다. |

<a id="deferred-join-barrier"></a>
### Deferred Join barrier

현재 handler가 정상적으로 끝난 뒤 Actor Join을 실행하고, 뒤에 들어온 Actor
message가 Join을 앞지르지 못하게 하는 process-local queue 경계다. `Defer()`를
호출한 시점에는 비활성 상태로 등록하며 target 조회나 Store I/O를 시작하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework 내부의 handler-scoped queue barrier |
| .NET 표기 | Public type 없음 |
| 공개 구성 | Current Actor identity·`ObjectGeneration`·membership, immutable join request snapshot, absolute deadline과 Actor Join `OperationId`에 결합한다. 내부 encoding은 공개 계약이 아니다. |
| 생성·관리 | 열린 handler registration scope에서 `Defer()`가 등록한다. Handler가 정상 종료하면 활성화하고 exception·cancellation·reply encoding failure이면 폐기한다. |
| 수명 | Registration부터 Join terminal과 completion ordering이 끝날 때까지 유지한다. Location commit 전 process가 종료되면 이 barrier 자체를 replay하지 않는다. |

<a id="bounded-aggregate-commit"></a>
### Bounded aggregate commit

Cross-node Actor Join처럼 서로 연관된 위치정보 여러 항목을 제한된 하나의 Store
transaction으로 함께 확정하는 commit 경계다. Actor owner만 먼저 바꾸고 membership
이나 capacity를 나중에 바꾸는 부분 상태를 공개하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Location Store의 bounded multi-record atomic transaction |
| .NET 표기 | 독립 public type 없음 |
| 공개 구성 | Actor authority, source·target membership, capacity와 aggregate generation을 함께 검증하고 변경한다. |
| 생성·관리 | Framework relocation coordinator가 target staging을 끝낸 뒤 한 번 실행한다. |
| 수명 | 성공한 commit이 logical relocation의 확정점이다. Callback, relay와 cleanup 완료를 기록하려고 같은 aggregate를 두 번째로 commit하지 않는다. |

<a id="message-follow"></a>
### Message Follow

Actor나 Spot이 다른 MeshNode로 relocation된 뒤에도 이전 owner node로 도착한
message를 새 owner에게 대신 전달하는 동작이다. 보내는 쪽이 옛 위치를 캐시하고
있어도 message를 잃지 않게 하는 것이 목적이며, 새 주소를 알려 주고 재전송을
요구하는 redirect가 아니다.

Relocation commit 뒤 이전 owner에 늦게 도착한 개별 message가 Message Follow 대상이다.
Message Follow는 무기한 유지하지 않고 [Message Follow duration](#message-follow-duration)
안에서만 유효하며, 이 기간이 끝난 뒤 도착한 message는 일반 stale route 실패로
처리한다.

Relocation 중 source가 seal한 뒤 보관하는
[relocation ingress hold](#relocation-ingress-hold)와는 다르다. Hold는 commit 전까지
source가 보관했다가 target queue로 넘기는 임시 저장이고, Message Follow는 commit이
끝나 owner가 바뀐 뒤 옛 owner에게 도착한 message를 처리한다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework가 관리하는 owner 이전 뒤 message 전달 |
| .NET 표기 | Public type 없음 |
| 공개 구성 | 새 owner의 `ActorRef` 또는 Spot 위치와 Message Follow 만료 시각을 유지한다. |
| 생성·관리 | Relocation commit이 끝난 뒤 이전 owner runtime이 만든다. |
| 수명 | Message Follow duration이 끝나면 제거하고, 이후 같은 위치로 온 message는 stale route 실패로 처리한다. |

<a id="message-follow-duration"></a>
### Message Follow duration

[Message Follow](#message-follow)가 유효한 기간이다. Relocation commit
시점부터 시작하며 이 기간이 지나면 이전 owner는 더 이상 전달하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework가 관리하는 기간 |
| .NET 표기 | `ZLinkLocationOptions.MessageFollowDuration` |
| 수명 | Relocation commit에서 시작해 만료로 끝난다. 만료 뒤 Message Follow 항목을 제거한다. |

<a id="relocation-ingress-hold"></a>
### Relocation ingress hold

Source Actor의 message 수락을 seal한 뒤에도 이전 source route로 도착한 message를
잃지 않도록 임시로 보관하는 크기가 제한된 queue다. `Defer()` 뒤 seal 전까지
도착한 message는 이 hold가 아니라 deferred Join barrier 뒤의 Actor queue에 둔다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework가 관리하는 bounded message hold |
| .NET 표기 | Public type 없음 |
| 공개 구성 | Message payload와 original operation identity, `ObjectGeneration`과 queue ordering에 필요한 Framework metadata를 유지한다. 내부 storage 형식은 공개하지 않는다. |
| 생성·관리 | Source runtime이 relocation seal 뒤 도착한 message를 보관한다. Capacity가 가득 차면 일반 messaging backpressure와 timeout을 적용한다. |
| 수명 | Commit 전 abort에서는 source queue로 원래 순서에 맞춰 되돌리고, commit 성공 뒤에는 target queue로 relay한 뒤 제거한다. |

<a id="reply-correlation"></a>
### Reply correlation

Request를 보낼 때 생성하여 request와 reply에 함께 유지하는 식별 정보다. 공개
계약에서는 여러 field의 조합이 아니라 `correlation_id`라는 단일 값으로 구성된다.

| 구분 | 계약 |
|---|---|
| 값 | `correlation_id` 하나 |
| .NET 표기 | Handler·monitoring context에서는 `string? CorrelationId`로 관측한다. One-way message에서는 `null`일 수 있다. |
| 형식 | Framework가 생성하는 1~64 byte의 opaque ASCII identifier |
| 생성 주체 | Request를 시작한 MeshNode, ClientServer client 또는 STREAM runtime |
| Uniqueness 범위 | 값을 만든 runtime의 같은 lifecycle에서 동시에 대기 중인 request 사이에서 중복할 수 없다. |
| 전달 | Request와 그 terminal reply 또는 error에 같은 값을 유지한다. One-way message에는 만들지 않는다. |
| 수명 | Request가 reply, error, timeout, cancellation 또는 shutdown으로 최종 완료될 때까지 유효하다. |
| Application 제약 | 값을 해석하거나 새 값을 조립하지 않는다. Application metadata key로 넣지 않는다. |

`flow_id`, target RID, endpoint, user ID와 payload는 `correlation_id`의 구성 요소가
아니다. `flow_id`는 여러 message가 이어진 업무 흐름을 관측하는 별도 값이며
request와 reply를 연결하는 기준으로 사용하지 않는다.

Client는 reply가 도착하면 `correlation_id`를 현재 대기 중인 request의 값과
비교한다. 값이 일치할 때만 해당 request의 결과로 처리하며, 일치하는 request가
없으면 늦게 도착한 reply로 판단하여 폐기한다.

Target이 Spot을 새로 준비하거나 current owner로 request를 전달해도 이 정보를
유지한다. Handler가 별도로 시작한 downstream request에는 원래 request와 다른 값을
사용한다.

전체 생성·전파 계약은 [Flow correlation](27-flow-correlation.ko.md)을 따른다.

<a id="deadline"></a>
### Deadline

작업을 끝내야 하는 마지막 시점이다. Request에서는 Spot 조회, cold activation,
handler 실행과 reply까지 하나의 end-to-end deadline을 적용할 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | Absolute end-to-end time boundary |
| .NET 표기 | Caller는 `TimeSpan` timeout을 지정하고 Framework와 lifecycle context는 고정된 `DateTimeOffset Deadline`을 사용한다. |
| 공개 구성 | Terminal submit을 시작할 때 한 번 계산한 마지막 시점 하나다. 단계별 timeout의 합이 아니다. |
| 생성·관리 | Source Framework가 caller timeout과 현재 시각으로 고정한다. |
| 수명 | Resolve, reservation, factory, Ready barrier, handler와 reply가 공유하며 terminal completion 뒤 폐기한다. |

<a id="factory"></a>
### Factory

등록된 stable type에 맞는 Spot instance를 생성하는 application 제공 코드다. 여러
target이 동시에 생성하려고 해도 생성 권한을 먼저 확보한 target만 factory를
실행한다.

| 항목 | 내용 |
|---|---|
| 형태 | Application-provided construction capability |
| .NET 표기 | Spot은 `AddSpotFactory<TSpot>`·`AddInstanceSpotFactory<TSpot>`, Actor는 `IZLinkActorFactory<TActor>` |
| 공개 구성 | Stable type, object 종류별 factory option, relocation policy와 concrete instance type을 registration에 결합한다. |
| 수명 | Object Server registration 동안 유지된다. Creation attempt에서 at-least-once 실행될 수 있으므로 retry-safe해야 한다. |

<a id="activation-barrier"></a>
### Activation barrier

Spot 초기화와 durable activation inbox의 첫 record 확정이 끝나기 전에 최초
application message가 handler로 전달되지 않게 막는 경계다. Framework는 recovery
root와 replay cursor를 유지한 `Ready` authority를 확정하고 첫 record를 queue
선두에 복원한 뒤 이 경계를 연다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework internal admission barrier |
| .NET 표기 | Public type 없음 |
| 공개 구성 | Initialize completion, durable inbox first record, Ready authority와 local queue-head restore 조건을 함께 검사한다. |
| 수명 | Cold activation 시작부터 조건을 모두 만족해 first handler admission을 열 때까지 유지된다. |

<a id="durable-activation-inbox"></a>
### Durable activation inbox

Instance Spot cold activation의 최초 application message를 process restart 뒤에도
복원할 수 있도록 저장하는 기록이다. Framework는 이 message를 첫 record로 확정한
뒤에만 Spot의 `Ready` authority를 게시한다.

| 항목 | 내용 |
|---|---|
| 형태 | Durable ordered record sequence |
| .NET 표기 | Public type 없음 |
| 공개 구성 | Inbox sequence, complete activation envelope와 처리 완료 상태를 보존한다. Provider는 envelope payload를 해석하지 않는다. |
| 생성·관리 | Target runtime이 Relocation Store에 first record를 확정하고 handler 완료를 durable하게 기록한다. |
| 수명 | Ready 전에 생성하며 first handler 완료와 replay cursor 갱신 뒤 recovery pointer가 해제될 때까지 복구 근거로 유지한다. |

<a id="replay-cursor"></a>
### Replay cursor

Durable activation inbox에서 처리가 끝났다고 durable하게 기록한 마지막 위치다.
Framework는 최초 handler 완료를 기록한 뒤 cursor를 해당 inbox sequence까지
갱신한다.

| 항목 | 내용 |
|---|---|
| 형태 | Monotonic inbox position |
| .NET 표기 | Public type 없음 |
| 공개 구성 | 마지막으로 terminal completion을 기록한 inbox sequence 하나다. 구체적인 encoding은 공개 계약이 아니다. |
| 생성·관리 | Target runtime이 handler terminal completion을 durable하게 기록한 뒤 갱신한다. |
| 수명 | Ready authority의 recovery pointer에 포함되며 pointer를 해제할 때까지 유지한다. 이전 위치로 되돌리지 않는다. |

<a id="activation-recovery-pointer"></a>
### Activation recovery pointer

Cold activation을 복구할 때 읽을 recovery root와 replay cursor를 `Ready` authority가
계속 가리키게 하는 정보다. 최초 handler 완료와 cursor 갱신이 끝나기 전에는 이
pointer를 제거하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Recovery root reference와 cursor의 복합 pointer |
| .NET 표기 | Public type 없음. Authority의 opaque Framework payload에 포함한다. |
| 공개 구성 | Immutable activation recovery root reference와 현재 replay cursor를 함께 가리킨다. |
| 생성·관리 | Ready commit이 authority payload에 기록하고 expected-version `Preserve` CAS로 제거한다. |
| 수명 | Ready Instance cold activation에만 존재한다. Creating·Closing·Relocating authority와 Actor·Entry·User Spot에는 둘 수 없다. |

<a id="recovery-receipt"></a>
### Recovery receipt

Relocation Store에 저장한 activation recovery root와 Pending creation authority의
연결을 확인하는 정보다. Location Store는 creation reservation과 함께 이를 기록하고
exact read에서 반환한다.

| 항목 | 내용 |
|---|---|
| 형태 | Immutable content 검증 record |
| .NET 표기 | Dedicated public type 없음. Provider 표면에서는 content reference, `ReadOnlyMemory<byte>` SHA-256과 encoded size로 표현한다. |
| 공개 구성 | Recovery root reference, SHA-256 hash와 encoded byte size를 포함한다. |
| 생성·관리 | Target이 root를 먼저 저장한 뒤 Location Store reservation에 원자적으로 연결한다. |
| 수명 | Ready commit 또는 fenced failure cleanup까지 Pending creation authority와 함께 유지한다. |

<a id="reservation-fence"></a>
### Reservation fence

특정 creation reservation만 계속 실행하거나 중단할 수 있게 provider가 발급한
식별 값이다. 이전 reservation에서 늦게 도착한 commit이나 abort가 현재 생성을
변경하지 못하게 한다.

| 항목 | 내용 |
|---|---|
| 형태 | Provider-issued 복합 reservation record |
| .NET 표기 | `ZLinkObjectReservation` |
| 생성·관리 | Location Store provider가 successful `Reserve`에서 발급한다. Commit과 Abort는 같은 fence를 exact 비교한다. |
| 수명 | 해당 Creating authority가 Ready로 commit되거나 exact abort로 정리될 때 닫힌다. |

```csharp
public sealed record ZLinkObjectReservation(
    ZLinkAuthorityKey Key,                  // Object kind와 global logical key의 authority key
    string StoreVersion,                    // Reserve가 만든 Creating authority version
    ulong ObjectGeneration,                 // 새 object incarnation
    ulong AuthorityOwnerGeneration,         // Initial authority owner generation
    string ReservationVersion,              // 이 reservation만 commit·abort할 수 있는 fence
    ZLinkMeshNodeDescriptorKey TargetDescriptor, // 선택한 target MeshNode identity
    ulong TargetNodeLifecycleGeneration,    // Target descriptor lifecycle fence
    ZLinkLocationOwnerToken TargetOwner);   // Target host owner lease fence
```

## 3. Message 호출과 비동기 실행

<a id="spot-direct"></a>
### Spot direct

Global Spot ID 하나를 지정하여 해당 Spot에 send 또는 request를 전달하는 방식이다.
Application은 owner RID나 endpoint를 지정하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | ID-addressed messaging surface |
| .NET 표기 | `IZLinkSpotClient.SendToSpot<T>()`, `RequestToSpot<T>()`; 결과 call은 `IZLinkSpotSendCall`, `IZLinkSpotRequestCall` |
| 공개 구성 | Global Spot ID, typed payload, optional metadata·timeout·Instance intent로 call을 구성한다. |
| 수명 | Single-use fluent call이며 terminal submit 뒤 다시 사용할 수 없다. |

<a id="spot-turn"></a>
### Spot turn

Spot callback 하나가 application queue에서 execution gate를 점유해 실행되는 단위다. 같은 execution
gate에서는 두 turn을 동시에 실행하지 않는다. `SpotWide` User Spot과 Instance Spot은 Spot 전체가 gate
하나를 사용한다. Entry Spot은 Spot lane과 Actor별 lane을 분리한다. `PerActor` User Spot은 Spot lane,
Actor별 lane과 timer별 lane을 분리하므로 서로 다른 gate의 turn은 동시에 실행할 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | Serialized callback execution unit |
| .NET 표기 | 독립 public type 없음. Spot handler와 lifecycle callback 실행 문맥으로 제공된다. |
| 공개 구성 | Callback 하나, callback이 들어간 application queue와 그 queue가 사용하는 execution gate ownership으로 구성된다. |
| 수명 | Callback 시작부터 completion까지 유지된다. `SpotWide` User Spot과 Instance Spot에서는 `Yield`로 shared Spot turn을 먼저 반환할 수 있다. |

<a id="async-yield"></a>
### Async와 Yield

- `Async`는 기다리는 동안 현재 Spot turn을 유지한다.
- `Yield`는 `SpotWide` User Spot 또는 Instance Spot의 shared turn을 반환하여 다음
  queue 작업을 실행할 수 있게 하고, 기다리던 결과가 확정되면 같은 Spot의 새
  turn에서 실행을 재개한다. 다른 실행 문맥에서는 사용할 수 없다.

| 항목 | 내용 |
|---|---|
| 형태 | Request call terminator 두 종류 |
| .NET 표기 | `ValueTask<TReply> Async<TReply>(...)`, `ValueTask<TReply> Yield<TReply>(...)` |
| 공개 구성 | 둘 다 같은 request 결과를 반환한다. `Yield`는 `SpotWide` User Spot과 Instance Spot에서만 유효하며 Channel·Spot·Actor request와 CPU·I/O worker call에만 제공한다. |
| 수명 | 한 fluent request call에서 terminal method 하나만 실행할 수 있다. |

Actor·Spot create·get-or-create는 request와 같은 결과를 반환하는 제한된 `Yield`를 제공한다. Actor join,
send, publish, timer 등록, close와 destroy에는 `Yield`를 제공하지 않는다. `SpotWide` member Actor가
`Yield`하면 shared User Spot gate만 반환하고 [Actor queue claim](#actor-queue-claim)은 현재 job이 끝날
때까지 유지한다.

<a id="submitted"></a>
### One-way 정상 완료

One-way call의 정상 완료는 source-local outbound admission이 operation을 수락했다는 뜻이다. Public
status나 result 값을 반환하지 않으며 target handler 실행이나 remote queue 수락을 확인하지 않는다.

<a id="backpressure"></a>
### Backpressure

송신 queue의 상한으로 송신 속도를 제한하는 흐름 제어다. Node는 상대별 송신 queue를
가지며, 아직 상대가 가져가지 않은 message가 차지하는 byte가 그 queue의 high-water mark에
닿으면 그 상대로 가는 새 제출을 잠근다. High-water mark는 message 개수가 아니라 그 queue가
보관하는 byte로 센다. 한도는 remote가 보내는 신호가 아니라 **자기 process 안의 값**이다. 다만 remote가 느리면 connection의 흐름 제어가 전송 속도를 낮추고 그만큼 송신
queue도 비워지지 않으므로, remote의 지연은 별도 신호가 아니라 송신 대기로 전달된다.

[One-way submit](05-async-execution-policy.ko.md#13-one-way-submit)이 비동기인 이유가 이
대기다. Capacity가 부족하면 Framework는 family별 send timeout까지 기다렸다가 정확히 한 번
제출하고, 그 안에 자리가 나지 않으면 [DeadlineExceeded](#deadlineexceeded)로 완료한다. 이때의
내부 상태를 [Backpressured](#backpressured)라 하며 public terminal result로 노출하지 않는다.

<a id="backpressured"></a>
### Backpressured

송신 경로나 queue의 capacity가 일시적으로 부족한 내부 상태다. Public terminal result가 아니며 Framework는
family별 send timeout까지 capacity를 기다린다. Logical Multicast를 시작한 뒤에는 target별
capacity 부족을 public 결과나 publish 전용 monitoring으로 집계하지 않는다.

<a id="application-hwm"></a>
### Application HWM

Framework가 이미 수신했지만 application handler가 아직 처리를 끝내지 않은 payload의 byte 합계를
제한하는 host 단위 값이다. Queue에서 기다리는 payload와 handler가 처리 중인 payload를 모두 센다.
이 합계가 상한에 도달하면 Framework는 새 application message 수신만 멈춘다. 이미 받은 job과
별도 Completion connection의 request reply·bounded Framework service control과 Core의 send-ready callback
처리는 계속하므로 message를 버리지 않고 송신 측에
[backpressure](#backpressure)가 전달된다.

`HWM`은 high-water mark의 약자다. 이 문서에서 별도 범위를 붙이지 않은 Application HWM은 Core
socket의 connection별 HWM과 다른 Framework host 전체 제한을 뜻한다.

<a id="timed-out"></a>
### DeadlineExceeded

Operation에 허용된 deadline까지 해당 operation의 완료 조건을 만족하지 못했을 때
발생하는 Framework exception이다. 완료 조건은 operation마다 다르다. 예를 들어
one-way send는 source queue가 message를 수락하는 시점, object 생성은 `Ready` 또는
생성 실패 결과가 확정되는 시점까지 기다린다.

Public submit status가 아니며 request handler가 application reply를 반환하지 못한
상태와도 구분한다.

<a id="target-not-found"></a>
### TargetNotFound

조건에 맞는 logical target을 찾거나 새로 준비할 수 없을 때 operation family가 발생시키는 오류 범주다.
Public Framework error kind는 `NotFound`다.

<a id="route-not-connected"></a>
### RouteNotConnected

Logical target은 확인했지만 현재 사용할 수 있는 송신 경로가 없을 때 발생하는 내부 transport
상태다. Public Framework error kind는 `Unavailable`이다.

<a id="shutdown"></a>
### Shutdown

Runtime이 종료를 진행하고 있어 새로운 operation admission을 받을 수 없는 상태다. 새 one-way call은
`ShuttingDown` exception으로 완료한다. Runtime termination reason과 outcome은 별도 lifecycle 결과가
소유한다.

## 4. Channel과 Logical Multicast

<a id="channelname"></a>
### ChannelName

Message를 보낼 Channel 범위를 식별하는 이름이다. Logical Multicast에서는 어떤
RouteMesh 참여 node를 remote target 후보로 볼지 결정한다.

| 항목 | 내용 |
|---|---|
| 형태 | 논리 Channel 이름 |
| .NET 표기 | `string` |
| 공개 구성 | 문자열 하나다. MeshName, socket과 endpoint를 포함하지 않는다. |
| 생성·관리 | Application이 topology와 handler를 등록할 때 지정한다. |
| 수명 | Process-local registration key로 유지된다. 같은 이름을 서로 다른 물리 topology에 중복 등록할 수 없다. |

<a id="topic"></a>
### Topic

같은 ChannelName 안에서 message를 받을 local Spot subscription을 고르는 값이다.
각 수신 node는 자신의 subscription만 검사한다.

| 항목 | 내용 |
|---|---|
| 형태 | Subscription selector |
| .NET 표기 | `string` |
| 공개 구성 | ChannelName과 별도로 전달하는 값 하나다. Spot ID나 remote node 목록을 포함하지 않는다. |
| 생성·관리 | Application이 subscription 등록과 publish call에 같은 값을 지정한다. |
| 수명 | 등록된 subscription 동안 유지된다. Classic fanout의 exact liveness topic은 application topic으로 사용할 수 없다. |

<a id="logical-multicast"></a>
### Logical Multicast

ChannelName과 topic으로 같은 Channel의 여러 Spot에 message 하나를 전달하는
방식이다. Framework는 remote node마다 message를 한 번 보내고, 각 node가 실제로
받을 local Spot을 결정한다.

| 항목 | 내용 |
|---|---|
| 형태 | Multi-target publish surface |
| .NET 표기 | `IZLinkSpotPublisherClient.Publish<T>()`, `IZLinkPublishCall` |
| 공개 구성 | ChannelName, topic, typed payload와 optional metadata를 입력으로 받고 결과값 없이 완료한다. |
| 수명 | 한 publish transaction에서 고정한 target에 제출을 시도하는 동안 유지된다. |

<a id="subscription"></a>
### Subscription

Spot이 특정 ChannelName, topic과 packet name에 해당하는 message를 받겠다고 등록한
정보다. 수신 node는 이 등록 정보가 일치하는 Spot queue에 message를 넣는다.

| 항목 | 내용 |
|---|---|
| 형태 | 복합 handler registration key |
| .NET 표기 | 독립 public type 없음. `IZLinkSpotHandlerRegistry.AddSubscribe<THandler>(string channelName, string topic)` 등록과 handler type으로 표현한다. |
| 공개 구성 | ChannelName, topic, message kind와 packet name의 조합이다. |
| 생성·관리 | Spot의 `Configure()`에서 application이 등록하고 Framework가 startup에 중복과 Channel membership을 검증한다. |
| 수명 | Spot handler registry lifecycle 동안 유지되며 같은 Spot에서 exact key를 중복 등록할 수 없다. |

<a id="snapshot"></a>
### Publish target snapshot

Publish를 시작할 때 고정한 remote target 목록과 source node에서 일치한 local Spot
목록이다. Publish 도중 참여 node가 바뀌어도 이미 시작한 작업의 snapshot은 바꾸지
않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Publish 시작 시 고정하는 target 집합 |
| .NET 표기 | Target identity와 count는 public 반환값으로 노출하지 않는다. |
| 공개 구성 | Positive-weight ready remote MeshNode 집합과 source node에서 일치한 local Spot 집합이다. |
| 생성·관리 | Framework가 publish transaction을 시작할 때 한 번 고정한다. |
| 수명 | 해당 publish의 target 제출이 끝날 때까지 유지되며 중간 membership 변경으로 바뀌지 않는다. |

<a id="relocation-policy"></a>
### Relocation policy

Actor나 Spot을 다른 node에서 계속 실행해야 할 때 application state를 어떻게
처리할지 factory registration에서 고정하는 정책이다.

| Policy | Target에서 유지하는 내용 |
|---|---|
| `DisableRelocation` | Cross-node relocation을 허용하지 않는다. Source owner와 application admission을 유지한다. |
| `RecreateOnRelocation` | Target factory로 application 객체를 다시 만든다. Framework queue·timer는 유지하지만 application state는 복원하지 않는다. 같은 logical incarnation이므로 `ObjectGeneration`은 유지한다. |
| `PreserveStateWith` | Handler가 정상 종료한 경계의 application state를 지정한 relocation adapter의 opaque byte sequence로 capture·restore한다. Framework queue·timer도 함께 유지한다. |

Application은 operation마다 policy를 바꾸지 못하며 startup 뒤 registration도 변경할 수 없다.

<a id="preserve-state-relocation-policy"></a>
### Preserve-state relocation policy

Actor나 Spot을 다른 node로 옮길 때 application state를 bytes로 저장하고 target의
새 instance에 복원하는 relocation policy다. Framework가 관리하는 queue, 아직
끝나지 않은 작업과 timer도 함께 옮긴다.

Factory configure callback의 `PreserveStateWith`가 adapter를 함께 지정한다.

<a id="classic-fanout"></a>
### Classic fanout

별도 PUB/SUB socket을 사용해 service event를 subscriber에게 전달하는 기능이다.
Spot Logical Multicast와 물리 연결이나 subscription 상태를 공유하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Independent PUB/SUB messaging surface |
| .NET 표기 | `IZLinkFanoutClient`, `IZLinkFanoutPublishCall`, `IZLinkFanoutHandler<TEvent>` |
| 공개 구성 | Fanout ChannelName, topic과 typed event를 사용하며 subscriber별 acknowledgement나 replay state는 갖지 않는다. |
| 수명 | Publisher·subscriber listener lifecycle과 각 publish admission 동안 유지된다. |

## 5. Queue, control과 수명

### Snapshot

특정 시점의 runtime 상태를 읽기 전용 값으로 복사한 결과다. Snapshot을 받은 뒤 실제 상태가
변경되어도 이미 반환된 값은 바뀌지 않는다. 따라서 monitoring이나 target 선택에서 Snapshot을
사용할 때는 “현재도 반드시 같은 상태”라는 보장으로 해석하지 않는다.

| 사용 위치 | Snapshot이 나타내는 것 |
|---|---|
| Monitoring | 조회한 시점의 node, channel, connection과 capacity 상태 |
| Publish target | publish를 시작할 때 고정한 수신 대상 집합 |
| Metadata | handler 또는 전송 call에 전달한 시점의 변경 불가능한 metadata 복사본 |

<a id="spot-application-queue"></a>
### Spot application queue

Spot direct payload, 일치한 Logical Multicast payload, timer callback과 Spot 상태를
바꾸는 control 작업을 순서대로 실행하는 queue다. Actor 업무 payload는 이 queue에
넣지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework-owned serialized queue |
| .NET 표기 | Public queue type 없음 |
| 공개 구성 | Spot direct, matching publish, timer와 Spot control work item을 한 순서로 보관한다. |
| 수명 | Spot incarnation 동안 유지되며 close·relocation에서는 lifecycle 규칙에 따라 seal·drain·restore한다. |

<a id="object-execution-queue"></a>
### Object execution queue

Actor 또는 Spot의 application 작업을 실행 순서대로 보관하는 Framework 내부 queue다.
Framework는 application instance를 찾기 전에 message의 object identity와 generation을
검사하고 이 queue를 찾는다. Create가 진행 중이면 application instance가 아직 없어도 queue가
먼저 존재할 수 있다. Relocation Restore는 아래 temporary queue를 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Object identity와 generation별 Framework-owned serialized queue |
| .NET 표기 | Public queue type 없음 |
| 공개 구성 | Create 같은 lifecycle 작업과 준비가 끝난 application object에 전달할 message의 실행 순서를 보관한다. |
| 수명 | Object 준비 전에 만들 수 있으며 같은 incarnation이 유지되는 동안 사용한다. 준비가 실패하거나 object가 제거되면 남은 작업을 terminal 결과로 끝낸 뒤 queue가 비었을 때 제거한다. |

Relocation 중에는 아직 준비되지 않은 target object의 message를 이 queue에 바로 넣지 않는다.
아래에서 정의하는 relocation temporary queue에 먼저 보관하고, target 준비가 끝나면 기존
작업 뒤에 옮긴다.

<a id="relocation-temporary-queue"></a>
### Relocation temporary queue

Target runtime이 Actor나 Spot을 복원하는 동안 해당 대상으로 들어오는 message를 잠시
보관하는 Framework 내부 queue다. Dispatch는 Actor나 Spot instance를 찾기 전에 현재
relocation에 등록된 temporary queue가 있는지 확인한다. 있으면 message를 그 queue에 넣고,
없으면 기존 dispatch 경로를 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | `RelocationId`, target attempt, object 종류·ID와 `ObjectGeneration`에 연결된 bounded Framework queue |
| .NET 표기 | Public queue type 없음 |
| 공개 구성 | Target identity, original operation identity, deadline, payload와 reply route를 보존한다. `SpotWide`에서는 Spot과 member Actor를 같은 relocation group에 넣되 record마다 실제 target을 보존한다. |
| 수명 | Target이 Restore 요청을 수락할 때 등록한다. Commit과 필요한 callback 뒤 실제 object queue로 작업을 옮기고 제거한다. Commit 전 abort에서는 실행하지 않고 폐기한다. |

Temporary queue에서 실제 object queue로 옮길 때 dispatch 전환을 atomic하게 처리한다. 전환
전에 수락한 message는 temporary queue에 남고, 전환 뒤 수락한 message는 실제 queue로 바로
들어간다. Framework는 저장했던 기존 작업을 실제 queue에 먼저 넣고 temporary queue의 작업을
그 뒤에 넣는다. 이 작업을 모두 마치기 전에는 실제 queue의 application handler를 실행하지
않는다.

<a id="spot-control-claim"></a>
### Spot control claim

Actor join, leave나 lifecycle 변경에 따라 Spot이 관리하는 상태를 바꾸는 control
작업이다. Target Spot의 다른 callback과 같은 queue 순서로 실행한다.

| 항목 | 내용 |
|---|---|
| 형태 | Spot queue control work item |
| .NET 표기 | 독립 public type 없음. Actor join·leave와 lifecycle API 결과로 표현한다. |
| 공개 구성 | Target Spot identity, control kind와 적용할 Actor·lifecycle 정보를 가진다. 내부 envelope는 공개하지 않는다. |
| 수명 | Spot application queue에서 해당 control callback이 완료될 때까지 유효하다. |

<a id="actor-queue-claim"></a>
### Actor queue claim

Actor queue head의 현재 job 하나를 실행할 권한이다. 같은 Actor에서 두 job이 겹치거나 뒤 job이 앞 job보다
먼저 실행되지 않게 한다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework-owned Actor queue execution claim |
| .NET 표기 | 독립 public type 없음. Actor handler 실행 문맥으로 제공된다. |
| 공개 구성 | Actor identity와 현재 queue head job을 결합한다. |
| 수명 | Handler 시작부터 continuation을 포함한 현재 job 완료까지 유지된다. `SpotWide` member Actor가 `Yield`해도 User Spot gate만 반환하며 이 claim은 유지한다. |

<a id="relocation-mode"></a>
### Relocation mode

Host의 stateful object를 어느 application version으로 이전할지 지정하는 caller intent다.
Application version을 유지하는 node 점검은 `PlannedMaintenance`, 준비한 새 version으로
전환하는 배포는 `RollingUpdate`를 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Closed value `PlannedMaintenance=0`, `RollingUpdate=1` |
| .NET 표기 | `ZLinkFrameworkRelocationMode` |
| 공개 구성 | `PlannedMaintenance`는 source와 같은 effective target version을 사용한다. `RollingUpdate`는 source보다 큰 exact `TargetApplicationVersion`을 함께 지정한다. |
| 수명 | 한 host `Relocate` operation을 시작할 때 고정하며 terminal result에도 같은 mode와 effective target version을 기록한다. |

Mode가 정한 exact version을 먼저 적용하고 그 뒤 capability, policy, adapter, capacity와
placement weight를 평가한다. 요청한 version과 다른 node는 더 높은 version이어도 target이
아니다.

<a id="relocation-unit"></a>
### Relocation unit

Host relocation에서 Framework가 source의 새 작업을 한 번 막고 target에 복원한 뒤 현재
처리 node를 바꾸는 최소 Actor 또는 Spot 묶음이다. 서로 다른 unit은 준비가 끝난 순서에
따라 동시에 이동할 수 있다. Unit 하나에 Actor와 Spot이 함께 포함되면 Actor를 처리할
node와 Actor가 속한 Spot을 해당 unit의 계약에 따라 함께 변경한다.

| 항목 | 내용 |
|---|---|
| 형태 | Actor 하나 또는 함께 이동해야 하는 Spot과 Actor의 묶음이다. 독립 public type은 없다. |
| 공개 구성 | Entry Spot Actor 하나, `PerActor` User Spot의 Actor 하나, `PerActor` Spot message target, `SpotWide` User Spot과 모든 member Actor, Instance Spot 하나 중 하나다. |
| 생성·관리 | Host `Relocate`를 처리하는 Framework가 현재 처리 node와 Spot execution mode를 기준으로 만든다. Application은 unit의 구성원을 추가하거나 제외하지 않는다. |
| 전달 | Relocation Store에는 unit의 identity, 저장한 state·queue·timer와 target 복원 정보를 기록한다. Application message에는 노출하지 않는다. |
| 수명 | Source가 새 작업을 막고 이동을 시작할 때 생성되며 위치 변경 후 target이 처리를 시작하거나 위치 변경 전 이동을 취소하면 끝난다. |
| Application 권한 | Application은 unit을 직접 만들거나 변경하지 않는다. `SpotWide`에서 `ApplicationSignaled`를 선택한 경우에만 이동을 시작할 안전한 turn을 알릴 수 있다. |

<a id="maintenance-wave"></a>
### Maintenance wave

같은 점검 작업에서 함께 종료하지 않아야 하는 host 묶음을 구분하는 application
설정값이다. Source와 target의 maintenance wave가 같으면 해당 target을 relocation
후보에서 제외한다.

값을 설정하지 않으면 이 제외 규칙을 사용하지 않는다. Framework는 설정된 문자열
전체를 대소문자를 구분하여 비교한다.

<a id="drain"></a>
### Drain과 draining

Drain은 host를 종료하기 위해 새로운 application 작업의 수락을 닫고, 이미 수락한
작업과 infrastructure resource를 정해진 시간 안에 정리하는 과정이다. 이 과정이
진행 중인 상태를 `draining` 또는 `drain 중`이라고 한다. Stateful object를 다른
host로 이전하는 relocation은 별도 operation이며 성공하면 host가 `Relocated`가 된다.

Drain을 시작했다고 해서 기존 connection을 즉시 끊거나 이미 수락한 작업을 바로
취소하지 않는다. 어떤 신규 작업을 차단하고 기존 작업을 언제까지 처리하는지는
component와 `Shutdown` 단계에 따라 달라진다. `Relocate`는 unit별 seal 전까지
기존 application 처리를 유지하고 성공해도 host를 종료하지 않는다.

ClientServer Server에서는 drain을 시작하면 새로운 send와 request의 target
선택에서 제외하고 새 업무 message의 수락을 중단한다. 이미 수락한 handler와
request reply는 deadline까지 처리한 뒤 descriptor, owner lease와 listener를
정리한다. 선택 비중만 `0`으로 바꾸고 Server 실행은 유지할 수 있는 Weight `0`과
달리, drain은 종료를 완료하는 lifecycle 절차다.

| 항목 | 내용 |
|---|---|
| 형태 | Lifecycle process와 closed state |
| .NET 표기 | `IZLinkFrameworkRuntime`, `ZLinkFrameworkRuntimeState`, `ZLinkFrameworkRuntimeEvent` |
| 공개 구성 | Mode와 exact target application version을 받는 `RelocateAsync`, 별도 `ShutdownAsync`, host runtime state와 deadline을 사용한다. |
| 수명 | `Shutdown` 시작부터 정상 정리 또는 force-stop 완료까지 진행하며 이미 수락한 작업의 deadline을 보존한다. |

<a id="drain-deadline"></a>
### Drain deadline

종료를 시작한 뒤 이미 수락한 작업과 lifecycle 정리를 끝내도록 허용한 시간이다.
종료가 시작되면 새로운 application payload는 받지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Absolute lifecycle deadline |
| .NET 표기 | `DateTimeOffset Deadline`; drain 시작 API에서는 `TimeSpan?`으로 기간을 지정할 수 있다. |
| 공개 구성 | Drain을 시작할 때 고정한 마지막 시점 하나다. |
| 생성·관리 | Framework가 caller가 지정한 기간 또는 기능별 기본값으로 계산한다. |
| 수명 | 이미 수락한 작업과 lifecycle cleanup이 이 시점까지 공유한다. Deadline 뒤에는 기능별 force-stop·fence 규칙을 적용한다. |

<a id="metadata-snapshot"></a>
### Metadata snapshot

업무 payload와 별도로 전달하는 작은 key-value 정보다. Framework는 변경할 수 없는
snapshot으로 고정하여 handler context에 제공한다. 실제 복사 시점과 내부 저장 방식은
공개 계약이 아니다.

| 항목 | 내용 |
|---|---|
| 형태 | Immutable key-value snapshot |
| .NET 표기 | `ZLinkMessageMetadata`와 `IReadOnlyDictionary<string, string>` |
| 공개 구성 | UTF-8 key와 value의 map이다. NUL을 허용하지 않으며 encoded key·value와 구조 overhead를 합쳐 최대 1024 byte다. |
| 생성·관리 | Outbound builder에서 application이 설정하고 Framework가 submit 때 immutable snapshot으로 고정한다. 같은 key는 마지막 값이 적용된다. |
| 수명 | Handler turn이 끝날 때까지 유효하다. 보관하려면 application이 복사해야 하며 request metadata를 reply에 자동 복사하지 않는다. |

`ZLinkMessageMetadata`의 공개 표면은 다음과 같다.

```csharp
public sealed class ZLinkMessageMetadata
{
    // 전달받은 key-value를 immutable metadata snapshot으로 만든다.
    public ZLinkMessageMetadata(
        IReadOnlyDictionary<string, string> values);

    public static ZLinkMessageMetadata Empty { get; } // 값이 없는 snapshot

    // 전체 key-value를 변경할 수 없는 view로 제공한다.
    public IReadOnlyDictionary<string, string> Values { get; }

    public string? Find(string key); // Key가 없으면 null을 반환한다.
}
```

## 6. RouteMesh와 Channel topology

<a id="membership"></a>
### Membership

Node나 Server가 특정 Mesh 또는 Channel에 참여한다는 등록 정보다. ChannelName
Server membership에는 handler와 target 선택에 사용할 weight가 포함된다.

| 항목 | 내용 |
|---|---|
| 형태 | 복합 topology registration |
| .NET 표기 | 독립 public type 없음. Builder registration과 monitoring의 `ZLinkMeshChannelSnapshot`으로 표현한다. |
| 공개 구성 | MeshName, ChannelName, Client/Server role과 Server일 때의 weight·handler namespace를 포함한다. |
| 생성·관리 | Application이 startup builder에서 등록하고 Framework가 descriptor와 process-local channel index에 반영한다. |
| 수명 | 해당 MeshNode 또는 ClientServer registration lifecycle 동안 유지된다. Drain과 weight 변경은 선택 가능 상태만 바꾼다. |

<a id="channel-client-server-role"></a>
### Channel Client와 Server role

- Client role은 Channel 호출을 시작할 송신 경로만 등록한다.
- Server role은 송신 경로와 remote target membership을 등록하고 handler와 weight를
  제공한다.

Server role은 Client의 송신 기능도 포함한다.

| 항목 | 내용 |
|---|---|
| 형태 | Closed registration role |
| .NET 표기 | ClientServer monitoring은 `ZLinkClientServerRole`; RouteMesh builder는 Client·Server 등록 method로 표현한다. |
| 공개 구성 | Client는 송신 경로만, Server는 송신 경로·target membership·handler namespace·weight를 등록한다. |
| 생성·관리 | Application이 같은 ChannelName에 역할별 최대 한 번 등록한다. |
| 수명 | Host startup configuration 동안 고정한다. Weight `0`이나 drain이 Server를 Client role로 바꾸지 않는다. |

<a id="weight"></a>
### Weight

여러 ready target 중 하나를 선택할 때 새 작업을 얼마나 자주 배정할지 정하는
`0..10000`의 상대 비중이다. 처리 가능한 동시 작업 수나 target의 물리적 성능을
뜻하지 않는다. 예를 들어 다른 조건이 같은 두 target의 weight가 `100`과 `50`이면
반복 선택에서 `100`인 target의 배정 비중을 두 배로 반영한다.

`0`이면 새로운 select-one과 Logical Multicast remote target에서 제외하지만 이미
제출한 작업을 취소하지 않는다. Weight를 `0`으로 바꾸는 것만으로 target이 drain
상태가 되거나 종료 절차를 시작하지 않는다. 실행 중에 weight를 다시 높이면 다른
조건을 만족하는 target은 선택 후보로 돌아올 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | Relative selection weight |
| .NET 표기 | `int` |
| 공개 구성 | `0..10000` 범위의 정수 하나이며 기본값은 `100`이다. 범위 밖 값은 startup 설정과 runtime 변경에서 configuration error다. |
| 생성·관리 | Application이 Server registration에 지정하고 허용된 runtime API로 변경한다. Descriptor revision이 변경을 순서화한다. |
| 수명 | Server lifecycle 동안 유지된다. `0`은 새 선택에서 제외할 뿐 role, connection과 이미 제출된 작업을 없애지 않는다. |

Node placement, RouteMesh Channel Server와 ClientServer Server가 같은 범위와 기본값을 사용한다. Weighted
selection은 후보 weight 합계를 최소 64-bit 정수로 계산한다. Logical Multicast는 positive weight의 크기와
관계없이 eligible remote member를 한 번 포함한다.

<a id="full-mesh"></a>
### Full mesh

같은 MeshName에서 서로 message를 주고받아야 하는 MeshNode pair를 직접 연결하는
topology다. Node가 `N`개이면 각 node가 관리하는 peer 연결은 최대 `N-1`개다.
두 node가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이
없으면 연결하지 않으므로 실제 연결 수는 이 상한보다 작을 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | RouteMesh connection topology |
| .NET 표기 | `IZLinkMeshNodeBuilder.PeerConnections` 설정과 `ZLinkMeshNodeSnapshot.Peers`로 구성·관측한다. |
| 공개 구성 | 같은 MeshName의 MeshNode 가운데 연결이 필요한 pair의 direct peer connection 집합이다. 양쪽 모두 Object Client이고 RouteMesh Channel Server membership도 없는 pair만 제외한다. |
| 수명 | MeshNode join·leave와 readiness에 따라 reconcile하며 각 connection은 독립 lifecycle을 가진다. |

<a id="peer-admission"></a>
### Peer admission

연결한 remote node의 MeshName, RID, lifecycle, descriptor, object role과 security
identity를 검사하여 ready peer 연결로 받아들일지 결정하는 과정이다. Manual
connection에서 양쪽이 Object Client이고 RouteMesh Channel Server membership도
없으면 connection이 필요하지 않다는 terminal admission 결과를 기록하고 ready 전에
socket을 닫는다.

| 항목 | 내용 |
|---|---|
| 형태 | Transport validation process |
| .NET 표기 | 독립 public result type은 없다. Peer 상태는 `ZLinkPeerStatus.State`로 관측하며 연결 장애 `NotConnected`와 정상 생략 `NotRequired`를 구분한다. |
| 공개 구성 | MeshName, RID, lifecycle generation, object role, descriptor 조건, protocol capability와 security identity 검증으로 구성된다. |
| 수명 | 새 connection마다 수행한다. 같은 manual endpoint와 configuration generation에서 Server membership 없는 Object Client pair로 끝난 결과는 연결 설정이 바뀔 때까지 재시도하지 않는다. |

`NotRequired`는 두 node가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server
membership이 없어 connection이 필요하지 않다는 뜻이다. Public monitoring에는 이
peer를 남기지만 ready·liveness·health failure 집계에서는 제외한다.
`NotConnected`는 연결이 필요한데 ready connection이 없다는 뜻이며 장애 집계에 반영한다.

<a id="lifecycle-generation"></a>
### Lifecycle generation

같은 logical server나 listener가 재시작되었을 때 이전 실행과 현재 실행을 구분하는
값이다. 이전 generation의 늦은 frame이나 reply를 현재 연결에 적용하지 않도록
사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | 0이 아닌 opaque equality token. 숫자 크기로 실행 순서를 판단하지 않는다. |
| .NET 표기 | `ulong LifecycleGeneration` |
| 공개 구성 | 0이 아닌 generation 값 하나다. Endpoint가 같아도 재시작한 실행은 이전 값과 다른 새 값을 사용한다. |
| 생성·관리 | Framework가 새 listener·server lifecycle에 사용할 값을 확정한다. |
| 수명 | 해당 실행이 끝날 때까지 유지된다. Remote는 descriptor와 transport admission의 값을 exact 비교한다. |

<a id="descriptor"></a>
### Descriptor

Remote runtime이 endpoint, identity, Channel membership, weight와 상태를 발견할 수
있도록 게시하는 등록 정보다. RouteMesh, ClientServer와 fanout은 서로 다른 종류의
descriptor를 사용한다. `Descriptor`는 이 등록 정보를 통칭하는 말이며, 실제
문서에서는 `MeshNode descriptor`, `ClientServer Server descriptor`, `fanout
publisher descriptor`처럼 어느 topology의 정보인지 함께 적는다.

| 항목 | 내용 |
|---|---|
| 형태 | Discovery record의 상위 개념 |
| .NET 표기 | 공통 base type은 없고 `ZLinkMeshNodeDescriptor`, `ZLinkClientServerServerDescriptor`, `ZLinkFanoutPublisherDescriptor`로 분리한다. |
| 공개 구성 | Identity, lifecycle, advertised endpoint, state와 exact owner lease를 공통으로 가지며 topology별 field는 각 descriptor 항목이 정한다. |
| 수명 | Host owner lease에 종속된 ephemeral record다. |

<a id="meshnode-descriptor"></a>
### MeshNode descriptor

RouteMesh의 automatic discovery에서 MeshNode가 다른 node에 자신의 identity와 접속
정보를 알리기 위해 Location Store에 게시하는 RouteMesh 전용 등록 정보다. 일반적인
객체 설명이 아니라, remote MeshNode가 peer 연결 후보를 찾고 검증할 때 사용하는
Framework 계약이다.

MeshNode descriptor에는 다음 정보가 들어간다.

- MeshName과 RID
- Lifecycle generation과 descriptor revision
- 실제로 연결할 advertised ROUTER endpoint
- Server ChannelName set과 Channel별 weight
- `None`, `Client`, `Server` 중 하나인 Object role
- 연결 상대를 검증하는 security identity
- Protocol version과 필수 capability
- Object Server이면 같은 lifecycle에 발급한 exact Entry Spot ID

| 항목 | 내용 |
|---|---|
| 형태 | 복합 discovery record |
| .NET 표기 | `ZLinkMeshNodeDescriptor`; key는 `ZLinkMeshNodeDescriptorKey` |
| 생성·관리 | MeshNode runtime이 listener bind와 admission 정보 확정 뒤 Location Store에 게시하고 owner lease로 갱신한다. |
| 수명 | Host owner lease에 종속된 ephemeral record다. Lifecycle이 바뀌면 새 generation의 descriptor를 게시한다. |

```csharp
public sealed record ZLinkMeshNodeDescriptor(
    string MeshName,                              // RouteMesh namespace
    RoutingId Rid,                               // MeshNode transport identity
    ulong LifecycleGeneration,                   // 현재 MeshNode 실행
    ulong DescriptorRevision,                    // 같은 lifecycle 안의 변경 순서
    string Endpoint,                             // 실제 advertised ROUTER endpoint
    string? EntrySpotId,                        // Object Server lifecycle의 exact Entry Spot ID
    IReadOnlyDictionary<string, int> ChannelWeights, // Server Channel별 selection weight
    string SecurityIdentity,                     // Transport peer admission identity
    string OwnerId,                              // Descriptor를 게시한 host owner
    long LeaseGeneration,                        // Host process lifecycle fence
    DateTimeOffset UpdatedAt)                    // Store에 기록한 갱신 시각
{
    public long ApplicationVersion { get; init; } // Application 배포 순번

    // Object kind·stable type·policy·placement capability
    public IReadOnlyList<ZLinkObjectCapability> ObjectCapabilities { get; init; }
        = Array.Empty<ZLinkObjectCapability>();

    public string? MaintenanceWave { get; init; } // Optional maintenance wave stable ID
    public ZLinkFrameworkRuntimeState State { get; init; } // Runtime 상태
    public ZLinkMeshNodeObjectRole ObjectRole { get; init; } // Object Client/Server 역할
    public int PlacementWeight { get; init; } = 100; // Object placement 선택 비중

}

public readonly record struct ZLinkMeshNodeDescriptorKey(
    string MeshName, // RouteMesh namespace
    RoutingId Rid);  // MeshNode transport identity
```

Descriptor의 capacity 정보는 Actor 전체, User·Instance Spot 전체와 Spot stable type별 active·reserved
count 및 설정한 limit을 서로 구분한 projection이다. 일반 object 전체를 하나로 합산한 active `10,000`
상한은 사용하지 않는다. Limit `0`은 제한 없음을 뜻하며 Entry Spot은 Spot capacity에 포함하지 않는다.
Descriptor 값은 후보를 빠르게 거르는 데만 사용하고, 실제 slot 확보 여부는 Location Store의 atomic
reservation으로 확정한다.

Remote MeshNode는 이 등록 정보에서 endpoint와 Object role을 확인한다. 두 descriptor의
Object role이 모두 `Client`이면 automatic connection intent를 만들지 않는다. 그 밖의
pair는 실제 transport handshake에서 MeshName, RID, lifecycle generation, Object role과
security identity가 등록 정보와 같은지 다시 확인하고 peer admission을 마쳐야 한다.

ClientServer Server, fanout publisher와 Spot·Actor location은 MeshNode descriptor에
기록하지 않는다. 각 기능은 자신의 descriptor나 location record를 사용한다.

<a id="clientserver-server-descriptor"></a>
### ClientServer Server descriptor

ClientServer Channel의 automatic discovery에서 Server가 Client에 자신의 identity,
접속 위치와 선택 상태를 알리기 위해 Location Store에 게시하는 등록 정보다.
Server는 이 등록 정보와 함께 owner lease를 게시한다.

다음 정보를 포함한다.

- ChannelName
- Server RID와 lifecycle generation
- 실제로 연결할 advertised endpoint
- Weight와 drain state
- Descriptor revision

| 항목 | 내용 |
|---|---|
| 형태 | 복합 discovery record |
| .NET 표기 | `ZLinkClientServerServerDescriptor`; key는 `ZLinkClientServerServerDescriptorKey` |
| 생성·관리 | ClientServer Server runtime이 listener와 selection 상태를 Location Store에 게시하고 owner lease로 갱신한다. |
| 수명 | Host owner lease에 종속된 ephemeral record다. Listener 재시작은 새 lifecycle generation을 사용한다. |

```csharp
public sealed record ZLinkClientServerServerDescriptor(
    string ChannelName,                 // Client가 조회할 service Channel
    RoutingId ServerRid,                // Server identity
    ulong LifecycleGeneration,          // 현재 Server 실행
    ulong DescriptorRevision,           // Weight·drain state 변경 순서
    string Endpoint,                    // 실제 advertised endpoint
    int Weight,                         // 새 request와 send의 상대 선택 비중
    ZLinkFrameworkRuntimeState State,   // Serving·draining 등 runtime 상태
    string SecurityIdentity,            // Transport admission identity
    string OwnerId,                     // Descriptor를 게시한 host owner
    long LeaseGeneration,               // Host process lifecycle fence
    DateTimeOffset UpdatedAt);           // Store에 기록한 갱신 시각

public readonly record struct ZLinkClientServerServerDescriptorKey(
    string ChannelName,   // ClientServer Channel
    RoutingId ServerRid); // Server identity
```

Owner lease는 Server가 이 등록 정보를 계속 사용할 권한이 있음을 정해진 시간마다
갱신하여 증명한다.

Client는 같은 ChannelName의 유효한 등록 정보에서 endpoint를 찾는다. 실제 transport
연결에서도 Server identity, lifecycle generation과 security identity가 등록 정보와
같은지 다시 확인해야 ready target으로 사용한다.

MeshName, RouteMesh membership과 Spot·Actor location은 ClientServer Server descriptor에
기록하지 않는다. ClientServer discovery에 MeshNode descriptor를 대신 사용하지도
않는다.

<a id="fanout-publisher-descriptor"></a>
### Fanout publisher descriptor

Classic fanout의 automatic discovery에서 publisher가 subscriber에 자신의 identity와
PUB endpoint를 알리기 위해 Location Store에 게시하는 등록 정보다. ChannelName,
Publisher RID, lifecycle generation과 advertised endpoint를 포함한다.

| 항목 | 내용 |
|---|---|
| 형태 | 복합 discovery record |
| .NET 표기 | `ZLinkFanoutPublisherDescriptor`; key는 `ZLinkFanoutPublisherDescriptorKey` |
| 생성·관리 | Publisher runtime이 PUB listener bind 뒤 Location Store에 게시하고 owner lease로 갱신한다. |
| 수명 | Host owner lease에 종속된 ephemeral record다. Publisher lifecycle이 바뀌면 새 generation을 사용한다. |

```csharp
public sealed record ZLinkFanoutPublisherDescriptor(
    string ChannelName,                // Fanout Channel
    RoutingId PublisherRid,            // Publisher identity
    ulong LifecycleGeneration,         // 현재 publisher 실행
    ulong DescriptorRevision,          // 같은 lifecycle 안의 변경 순서
    string Endpoint,                   // Subscriber가 연결할 advertised PUB endpoint
    ZLinkFrameworkRuntimeState State,  // Publisher runtime 상태
    string SecurityIdentity,           // 연결 admission identity
    string OwnerId,                    // Descriptor를 게시한 host owner
    long LeaseGeneration,              // Host process lifecycle fence
    DateTimeOffset UpdatedAt);          // Store에 기록한 갱신 시각

public readonly record struct ZLinkFanoutPublisherDescriptorKey(
    string ChannelName,      // Fanout Channel
    RoutingId PublisherRid); // Publisher identity
```

Subscriber는 같은 fanout ChannelName의 publisher descriptor만 조회한다. MeshNode나
ClientServer Server descriptor를 fanout 연결 정보로 사용하지 않는다. Publisher
endpoint마다 별도의 SUB socket을 만들고 각 연결의 ready와 liveness를 독립적으로
판단한다.

<a id="descriptor-revision"></a>
### Descriptor revision

같은 lifecycle 안에서 weight나 drain state처럼 변경 가능한 descriptor 정보의
version을 나타내는 1 이상의 증가하는 번호다. 더 낮은 revision으로 현재 상태를
되돌리지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Monotonic revision |
| .NET 표기 | `ulong DescriptorRevision` |
| 공개 구성 | 1 이상의 정수 하나다. Lifecycle generation과 함께 비교한다. |
| 생성·관리 | Descriptor owner가 같은 lifecycle에서 공개 상태를 변경할 때 증가시킨다. |
| 수명 | 해당 lifecycle 안에서만 순서를 비교한다. 새 lifecycle의 revision과 이전 lifecycle의 값을 직접 비교하지 않는다. |

<a id="automatic-discovery"></a>
### Automatic discovery

Location Store에 게시된 descriptor를 조회하여 remote endpoint와 identity를 찾는
방식이다. Descriptor를 찾은 뒤 실제 transport 연결에서도 identity와 lifecycle
generation을 확인해야 ready target으로 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Descriptor-based discovery process |
| .NET 표기 | `IZLinkLocationStore` 등록과 endpoint를 생략한 topology builder 설정으로 사용한다. |
| 공개 구성 | Descriptor query, Server membership 없는 Object Client pair 제외, desired-set reconcile, transport connect와 identity·lifecycle admission 단계로 구성된다. |
| 수명 | Host runtime이 store polling과 connection lifecycle 동안 반복 수행한다. |

<a id="manual-discovery"></a>
### Manual endpoint

Application 설정으로 remote endpoint를 직접 등록하는 방식이다. Endpoint만으로
신뢰하지 않고 실제 연결의 identity와 설정 조건을 다시 확인한다.

| 항목 | 내용 |
|---|---|
| 형태 | Application-provided endpoint configuration |
| .NET 표기 | Topology별 builder의 `Listen(string)`·manual peer·subscriber endpoint method에서 `string`으로 지정한다. |
| 공개 구성 | Remote endpoint와 topology가 요구하는 expected identity 조건으로 구성된다. 양쪽 모두 Object Client이고 RouteMesh Channel Server membership도 없는 pair는 handshake admission에서 ready 전에 제외한다. |
| 수명 | Host startup configuration 동안 고정한다. 연결이 필요한 pair의 reconnect는 transport admission을 다시 수행한다. 같은 endpoint와 configuration generation에서 제외한 pair는 설정이 바뀔 때까지 다시 연결하지 않는다. |

<a id="ready-target"></a>
### Ready target

Listener와 transport 연결, identity 검사 및 필요한 handler 등록이 끝나 새로운
message를 받을 수 있는 target이다.

| 항목 | 내용 |
|---|---|
| 형태 | Selectable runtime target state |
| .NET 표기 | `ZLinkMeshPeerSnapshot.Ready`, `ZLinkClientServerServerSnapshot.Ready`와 기능별 state enum |
| 공개 구성 | Transport가 준비되었고 identity·lifecycle 검사를 통과했으며 필요한 handler·role 조건을 만족한다. Select-one 후보는 여기에 positive weight와 non-draining 조건도 만족해야 한다. |
| 수명 | 조건 중 하나가 닫히면 새 target 선택에서 즉시 제외된다. |

<a id="max-message-size"></a>
### MaxMessageSize

Listener가 받을 수 있는 complete transport message의 byte 상한이다. 상한을 넘긴
message의 일부 payload를 handler에 전달하지 않는다. 일반 application listener는
자신이 소유한 socket option의 값을 사용한다. StreamNode는 이 일반 listener 설정과
별도로 Core STREAM inbound에 적용하는 전용 규칙을 가진다.

StreamNode의 `MaxMessageSize`는 client에서 server로 들어오는 complete message의
header와 payload 합을 검사하며 6-byte prefix는 포함하지 않는다. 기본값은 `64 KiB`
(`65,536` bytes)다. `0`은 별도 Framework 상한을 사용하지 않고 Core의
`ZLINK_OPT_MAXMSGSIZE = -1`로 변환한다. 양수는 유한한 상한이고 음수는 startup
configuration error다. 이 상한은 server에서 client로 보내는 outbound message에는
적용하지 않는다. 상한을 넘은 message는 일부도 session handler에 전달하지 않으며,
server는 `EMSGSIZE`를 기록하고 연결을 종료한다. raw client에는 별도 error code를
보내지 않고 연결 종료만 관찰된다.

| 항목 | 내용 |
|---|---|
| 형태 | Byte-size configuration limit |
| .NET 표기 | 일반 socket은 `long MaxMessageSize`, StreamNode는 `ConfigureSocket().MaxMessageSize`다. |
| 공개 구성 | Listener가 소유한 socket의 complete message에 적용한다. 일반 socket의 `0`은 binding 또는 transport 기본값을 사용하고, StreamNode의 `0`은 Core `-1`로 변환한다. |
| 생성·관리 | Application이 startup 전에 해당 listener 또는 StreamNode socket option에 지정한다. |
| 수명 | Listener lifecycle 동안 고정되며 StreamNode 값은 startup 뒤 바뀌지 않는다. StreamNode 전용 상한은 ClientServer listener와 RouteMesh SS transport에 추가하지 않으며, ClientServer는 일반 application listener 규칙을 유지한다. |

## 7. Channel 메시징

<a id="node-direct"></a>
### Node direct

Caller가 MeshName과 target RID를 함께 지정하여 특정 MeshNode에 message를 보내는
방식이다. Framework는 지정한 RID를 다른 node로 바꾸지 않는다. Object Client는
application Node direct handler를 등록할 수 없으므로 Node direct target이 아니다.

| 항목 | 내용 |
|---|---|
| 형태 | Explicit node-addressed messaging surface |
| .NET 표기 | `IZLinkRouteClient`의 node direct send/request call |
| 공개 구성 | MeshName, Object Client가 아닌 target `RoutingId`, typed payload와 optional metadata·timeout으로 구성한다. |
| 수명 | Single-use call이며 지정한 RID의 route가 없다고 다른 node로 변경하지 않는다. |

<a id="select-one"></a>
### Select-one

ChannelName에 참여한 여러 ready Server 중 현재 호출을 받을 하나를 선택하는
방식이다. Weight, ready와 drain 상태를 함께 반영하고 선택한 target에 같은 작업에서
message를 제출한다.

| 항목 | 내용 |
|---|---|
| 형태 | Atomic target selection and submit operation |
| .NET 표기 | Channel send/request call builder 내부 동작이며 선택된 Server identity를 중간 결과로 반환하지 않는다. |
| 공개 구성 | Process-local ChannelName route, eligible ready target snapshot, weight와 drain filter로 구성된다. |
| 수명 | 한 submit operation 안에서 target 하나를 선택하고 message를 제출할 때 끝난다. |

<a id="handler-namespace"></a>
### Handler namespace

같은 packet name을 어느 handler 등록 범위에서 찾을지 구분하는 영역이다. Node
direct와 ChannelName handler는 서로 다른 namespace를 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | 복합 handler lookup scope |
| .NET 표기 | 독립 public type 없음. Handler registry와 context type으로 구분한다. |
| 공개 구성 | Node direct는 MeshName, message kind와 packet name을 사용하고 Channel handler는 ChannelName, message kind와 packet name을 사용한다. |
| 생성·관리 | Framework가 startup handler registration으로 index를 만든다. |
| 수명 | Host handler registry lifecycle 동안 유지되며 같은 namespace의 exact key를 중복 등록할 수 없다. |

<a id="message-kind"></a>
### Message kind

Send, request나 publish처럼 message가 어떤 처리 방식을 사용하는지 구분하는 값이다.
Handler를 찾을 때 ChannelName 또는 MeshName, packet name과 함께 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Closed message category |
| .NET 표기 | Handler type으로 구분하며 monitoring에서는 `ZLinkDispatchMessageKind`를 사용한다. |
| 공개 구성 | Send, Request와 Publish처럼 completion·reply 방식이 다른 category 하나다. |
| 생성·관리 | Framework가 호출한 send/request/publish surface에서 결정한다. |
| 수명 | Message envelope와 handler lookup 동안 바뀌지 않는다. |

<a id="packet-name"></a>
### Packet name

같은 handler namespace에서 typed handler를 선택하는 message 이름이다. 같은
namespace에 message kind와 packet name이 모두 같은 handler를 중복 등록할 수 없다.

| 항목 | 내용 |
|---|---|
| 형태 | Typed handler selector |
| .NET 표기 | 기본 이름은 Framework가 message type에서 결정하는 `string`이다. Stream Connector에서는 `IZlinkStreamSendCall.PacketName(string)`과 `IZlinkStreamRequestCall.PacketName(string)`으로 호출별 이름을 지정할 수 있다. |
| 공개 구성 | Handler namespace와 message kind 안에서 비교하는 이름 하나다. Payload bytes나 correlation을 포함하지 않는다. |
| 생성·관리 | Framework가 typed message 등록에서 확정한다. 해당 public contract가 override를 허용하는 경우에는 application이 호출별 이름을 지정할 수 있다. Codec은 packet name을 결정하지 않는다. |
| 수명 | 해당 message와 handler registration 동안 stable해야 한다. |

<a id="liveness-beacon"></a>
### Liveness와 liveness beacon

Liveness 확인은 연결 상대의 신호가 정해진 시간 안에 계속 도착하는지 확인하여
connection을 ready 상태로 유지할 수 있는지 판단하는 동작이다. Application
message의 handler 실행이나 업무 처리 성공을 확인하는 기능은 아니다.

Liveness beacon은 단방향 연결의 liveness를 확인할 수 있도록 runtime이
주기적으로 보내는 내부 message다. Classic fanout의 연결 상태 확인용 topic과
beacon은 application publish나 handler에 노출하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Exact two-frame internal multipart record |
| .NET 표기 | Public type 없음 |
| 공개 구성 | Topic frame `01 5A 4C 46 31`과 payload frame `5A 46 01 01`의 정확히 두 frame이다. |
| 생성·관리 | Fanout publisher runtime이 application publish와 무관하게 5초마다 보낸다. |
| 수명 | Subscriber는 exact record만 liveness 신호로 처리한다. Topic이 같지만 payload나 frame 수가 다르면 protocol error이며 application handler로 전달하지 않는다. |

## 8. ClientServer Channel

<a id="clientserver-channel"></a>
### ClientServer Channel

Client가 send와 request를 시작하고 Server가 handler 실행과 reply를 담당하는
단방향 service 경계다. Server는 Client를 대상으로 새로운 업무 호출을 시작하지
않는다. 한 process는 같은 ChannelName에 Client와 Server를 각각 한 번 등록할 수
있다. Monitoring의 `client_and_server`는 두 registration이 함께 있다는 snapshot
표현이며 별도의 builder role이 아니다.

| 항목 | 내용 |
|---|---|
| 형태 | Client-initiated service topology |
| .NET 표기 | `IZLinkClientServerChannelRoleBuilder`, `IZLinkClientServerRuntime`, `ZLinkClientServerChannelSnapshot` |
| 공개 구성 | ChannelName, Client·Server registration, Server descriptor·connection set, handler namespace와 select-one state로 구성된다. |
| 수명 | Host registration lifecycle 동안 유지되며 Client와 Server 역할은 각각 최대 한 번 등록한다. |

<a id="server-identity"></a>
### Server identity

ClientServer 연결에서 특정 Server 실행을 식별하는 값이다. Lifecycle generation과
함께 확인하여 재시작 전의 오래된 연결을 새 target으로 사용하지 않게 한다.

| 항목 | 내용 |
|---|---|
| 형태 | Server RID와 lifecycle의 복합 identity |
| .NET 표기 | `ZLinkClientServerServerDescriptorKey(ChannelName, ServerRid)`와 `ulong LifecycleGeneration` |
| 공개 구성 | ChannelName, `RoutingId` Server RID와 lifecycle generation을 함께 사용한다. Endpoint는 identity가 아니다. |
| 생성·관리 | Server runtime이 listener lifecycle 시작 시 확정하고 descriptor와 transport admission에 같은 값을 제공한다. |
| 수명 | 해당 Server lifecycle 동안 유효하다. 재시작하면 endpoint가 같아도 새 lifecycle generation을 사용한다. |

<a id="reply-token"></a>
### Reply token

Server request handler가 현재 request에 reply할 수 있도록 Framework가 제공하는
한 번만 사용할 수 있는 권한이다. 최종 reply를 만든 뒤 다시 사용할 수 없다.

| 항목 | 내용 |
|---|---|
| 형태 | One-shot reply capability |
| .NET 표기 | 독립 token type을 application에 노출하지 않고 typed request handler의 단일 reply 반환으로 표현한다. |
| 공개 구성 | Current request correlation과 terminal-once 상태에 연결된 opaque capability다. 내부 handle과 route는 공개하지 않는다. |
| 생성·관리 | Framework가 request dispatch 시 만들고 handler completion을 terminal reply로 변환한다. |
| 수명 | 첫 terminal reply, error 또는 request cancellation·timeout으로 닫힌다. 닫힌 뒤 재사용할 수 없다. |

<a id="downstream-request"></a>
### Downstream request

Handler가 원래 request를 처리하는 중 다른 RouteMesh, ClientServer Channel, Spot
또는 Actor에 새로 보내는 request다. 원래 request와 다른 correlation을 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Handler-originated independent request |
| .NET 표기 | 해당 target surface의 `IZLinkRequestCall`, `IZLinkSpotRequestCall` 또는 `IZLinkActorRequestCall` |
| 공개 구성 | 새 target, typed payload, 새 reply correlation과 optional metadata·timeout으로 구성된다. |
| 수명 | 원래 request와 독립된 terminal completion을 가지며 결과를 원래 correlation으로 바꾸지 않는다. |

<a id="owner-lease"></a>
### Owner lease

Framework host가 현재 lifecycle의 등록 정보와 object ownership을 계속 사용할
권한이 있음을 정해진 시간마다 갱신하여 증명하는 정보다. 같은 host token이
MeshNode·ClientServer Server·fanout publisher descriptor, automatic RID claim,
Actor·Spot authority와 maintenance role을 fence한다.

| 항목 | 내용 |
|---|---|
| 형태 | Owner token과 expiry의 복합 lease record |
| .NET 표기 | `ZLinkLocationOwnerToken`, `ZLinkOwnerLeaseReadResult.Found` |
| 생성·관리 | Location Store provider가 claim 시 generation을 발급하고 host가 정해진 주기로 renew한다. |
| 수명 | `LeaseExpiresAt`까지 유효하다. Exact owner token이 다르거나 store 기준 expiry를 지나면 stale이다. |

Owner identity는 다음 `ZLinkLocationOwnerToken`으로 묶는다.

```csharp
public readonly record struct ZLinkLocationOwnerToken(
    string OwnerId,         // Host owner identity
    long LeaseGeneration);  // 같은 OwnerId의 process lifecycle fence
```

Lease를 읽은 결과인 `ZLinkOwnerLeaseReadResult.Found`는 token과 시각을 함께 반환한다.

```csharp
public abstract record ZLinkOwnerLeaseReadResult
{
    private protected ZLinkOwnerLeaseReadResult() { }

    public sealed record Found(
        ZLinkLocationOwnerToken Token, // 현재 exact owner identity와 generation
        DateTimeOffset LeaseExpiresAt, // Store 기준 만료 시점
        DateTimeOffset StoreNow)       // Expiry를 판단할 provider 기준 시각
        : ZLinkOwnerLeaseReadResult;

    public sealed record Missing : ZLinkOwnerLeaseReadResult;
}
```

<a id="fencing-deadline"></a>
### Fencing deadline

Owner lease 갱신에 실패한 Server가 새로운 업무 message 수락을 중단해야 하는
마지막 시점이다.

| 항목 | 내용 |
|---|---|
| 형태 | Absolute lease-derived deadline |
| .NET 표기 | Public standalone type 없음. 계산 결과는 `DateTimeOffset` 시점이다. |
| 공개 구성 | 마지막 valid owner lease deadline에서 fencing margin을 반영한 시점 하나다. |
| 생성·관리 | Framework가 마지막으로 확인한 valid lease와 location option으로 계산한다. |
| 수명 | 새 valid lease를 확인하면 갱신된다. Deadline에 도달하면 Store failure grace와 관계없이 새 업무 admission을 닫는다. |

## 9. Network listener

<a id="network-listener"></a>
### Network listener

현재 process에서 network endpoint를 bind하고 remote 연결을 받는 transport
구성 요소다.

| 항목 | 내용 |
|---|---|
| 형태 | Runtime transport component |
| .NET 표기 | Process 기본값은 `IZLinkNetworkOptions`; listener는 RouteMesh·ClientServer·fanout·STREAM builder가 구성한다. |
| 공개 구성 | BindHost, configured 또는 allocated port, AdvertiseHost, actual advertised endpoint와 topology identity를 가진다. |
| 수명 | Bind 성공부터 drain·shutdown으로 listener를 닫을 때까지 유지된다. |

<a id="bind-host"></a>
### BindHost

Listener가 현재 host의 어느 local network interface에서 연결을 받을지 지정하는
주소다. Wildcard 주소를 사용할 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | Host/address configuration value |
| .NET 표기 | `string` |
| 공개 구성 | Host 이름 또는 IP address 하나다. Process 기본값은 `127.0.0.1`이다. |
| 생성·관리 | Application이 process 기본값이나 listener override로 지정한다. |
| 수명 | Listener bind 전에 확정한다. `0.0.0.0`과 `::`를 사용할 수 있지만 remote advertised address로 내보내지 않는다. |

<a id="advertise-host"></a>
### AdvertiseHost

Remote process가 listener에 실제로 연결할 때 사용하는 host 또는 주소다. Remote가
연결할 위치를 확정할 수 없는 wildcard 주소는 사용할 수 없다.

| 항목 | 내용 |
|---|---|
| 형태 | Host/address configuration value |
| .NET 표기 | `string` |
| 공개 구성 | Remote에서 해석할 수 있는 host 이름 또는 IP address 하나다. |
| 생성·관리 | Application이 지정하거나 non-wildcard BindHost에서 Framework가 같은 host를 사용한다. |
| 수명 | Descriptor를 게시하기 전에 확정한다. 값이 바뀐 listener 재시작은 새 lifecycle generation을 사용한다. |

<a id="wildcard-address"></a>
### Wildcard address

`0.0.0.0` 또는 `::`처럼 여러 local network interface에서 연결을 받는 bind
주소다. Local BindHost에는 사용할 수 있지만 AdvertiseHost에는 사용할 수 없다.

| 항목 | 내용 |
|---|---|
| 형태 | Special bind address value |
| .NET 표기 | `string` |
| 공개 구성 | IPv4 `0.0.0.0` 또는 IPv6 `::` exact value다. |
| 수명 | Local bind 입력에만 사용할 수 있고 descriptor·manual remote endpoint에는 남길 수 없다. |

<a id="advertised-endpoint"></a>
### Advertised endpoint

AdvertiseHost와 실제 bound port를 결합하여 remote process에 제공하는 접속
주소다. Wildcard host나 port `0`이 남아 있으면 안 된다.

| 항목 | 내용 |
|---|---|
| 형태 | Host와 port의 복합 접속 주소 |
| .NET 표기 | Public descriptor에서는 `string Endpoint` |
| 공개 구성 | `AdvertiseHost + actual bound port`다. URI scheme 등 추가 형식은 해당 transport 계약이 정한다. |
| 생성·관리 | Framework가 listener bind 뒤 실제 port를 읽어 확정하고 topology별 descriptor에 기록한다. |
| 수명 | Listener lifecycle 동안 유지된다. Host나 actual port가 바뀌면 새 lifecycle generation과 함께 게시한다. |

<a id="routing-id"></a>
### Routing ID

같은 RouteMesh 안에서 MeshNode를 식별하는 byte 값이다. Automatic discovery에서는
Framework가 lifecycle마다 새 값을 만들고 manual topology에서는 명시적 fixed RID를
사용할 수 있다.

| 항목 | 내용 |
|---|---|
| 형태 | Opaque byte identifier |
| .NET 표기 | `RoutingId` |
| 공개 구성 | Full RID는 최대 255 byte다. Core raw socket automatic RID는 16-byte binary UUID v4다. Diagnostic prefix를 제공하는 Framework automatic RID는 prefix와 36자리 lowercase canonical UUID v4로 구성한다. |
| 생성·관리 | Automatic mode에서는 Framework가 lifecycle마다 만든다. Active conflict에서는 새 UUID나 두 번째 claim을 만들지 않는다. Manual topology에서는 application이 fixed RID를 지정할 수 있다. |
| 수명 | MeshNode lifecycle 동안 바뀌지 않는다. Replacement lifecycle은 endpoint가 같아도 새 Automatic RID를 사용한다. |

Transport RID와 Spot ID의 발급 형식과 namespace 경계는
[시스템 전체 Routing ID 정책](10-network-listener-identity.ko.md#7-시스템-전체-transport-rid와-spot-id-정책)을 따른다.

<a id="routing-id-prefix"></a>
### Routing ID prefix

Automatic MeshNode RID 앞부분에 붙이는 진단용 문자열이다. Application identity,
placement, shard나 재시작 뒤에도 유지되는 host 이름으로 사용하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Optional diagnostic string |
| .NET 표기 | `string?` |
| 공개 구성 | ASCII `[A-Za-z0-9._-]` 1~64자다. Framework가 `prefix-<lowercase-canonical-uuid-v4>`를 만든다. |
| 생성·관리 | Application이 startup 설정에 지정하고 Framework가 Automatic RID 생성에 사용한다. |
| 수명 | 해당 RID 생성에만 사용한다. Stable application identity나 placement key로 해석하지 않는다. |

<a id="csprng"></a>
### CSPRNG

다음 값을 예측하기 어려운 cryptographically secure pseudo-random number generator다.
Framework는 UUID v4의 random bit를 만들 때 platform cryptographic random API를 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Framework internal randomness capability |
| .NET 표기 | Public Framework type 없음. 구현은 platform cryptographic random API를 사용한다. |
| 공개 구성 | UUID v4의 version·variant bit를 제외한 random bit를 생성한다. UUID의 public 표현은 lowercase canonical 문자열 또는 Core의 16-byte binary 값이다. |
| 수명 | RID generation operation마다 새 값을 생성하며 random state를 application에 노출하지 않는다. |

<a id="routing-id-conflict"></a>
### RoutingIdConflict

Framework가 자동 발급한 RID를 claim할 때 이미 active identity가 사용 중임을 확인한 결과다. UUID 충돌은
정상적인 운영 상황으로 간주하지 않으므로 새 UUID를 만들어 재시도하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Startup configuration failure |
| .NET 표기 | `ZLinkConfigurationException` |
| 공개 구성 | Active transport descriptor owner claim이 충돌했다는 설명을 제공한다. 충돌한 owner token은 포함하지 않는다. |
| 수명 | 해당 startup operation을 terminal failure로 끝내며 같은 operation에서 새 UUID를 만들지 않는다. |

<a id="spot-id-conflict"></a>
### SpotIdConflict

Global Spot ID namespace에서 Entry·User·Instance Spot identity claim이 이미 사용 중임을 확인한 결과다.
Framework는 기존 claim을 덮어쓰거나 새 UUID를 만들어 같은 operation을 다시 시도하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Startup 또는 create failure |
| .NET 표기 | Startup은 `ZLinkConfigurationException`, exclusive create는 `ZLinkFrameworkErrorKind.AlreadyExists` |
| 공개 구성 | Global Spot ID claim이 충돌했음을 설명한다. 충돌한 owner token은 포함하지 않는다. |
| 수명 | 해당 startup 또는 create operation을 terminal failure로 끝낸다. |

## 10. STREAM session과 Actor binding

<a id="stream-session"></a>
### STREAM session

STREAM client connection 하나를 수락한 때부터 닫을 때까지 유지하는 서버 실행
단위다. Typed packet handler, request correlation, backpressure와 close 처리를 이
단위에 연결한다.

| 항목 | 내용 |
|---|---|
| 형태 | Runtime session object |
| .NET 표기 | `IZLinkSession`; context는 `IZLinkSessionContext` |
| 공개 구성 | Context가 `SessionId`, optional `RoutingId`, local·remote address, outbound client, Actor binding과 handler registry를 제공한다. |
| 생성·관리 | Framework가 STREAM connection을 수락할 때 session instance를 만들고 lifecycle callback을 실행한다. |
| 수명 | Connection을 수락한 때부터 disconnect·close가 끝날 때까지 유지한다. 재연결은 이전 session identity와 binding state를 재사용하지 않는다. |

<a id="binding-token"></a>
### Binding token

Actor와 현재 STREAM session의 binding을 식별하고, 재연결 뒤 늦게 도착한 이전
session 작업을 구분하는 값이다.

| 항목 | 내용 |
|---|---|
| 형태 | Opaque one-binding token |
| .NET 표기 | Application에 독립 public scalar type으로 노출하지 않는다. Binding API가 내부 token을 소유한다. |
| 공개 구성 | 단일 opaque token이다. 내부 encoding은 공개 계약이 아니다. Binding 관계는 exact `ActorRef`, current authority·lease generation, session identity와 binding generation을 함께 검증한다. |
| 생성·관리 | Current Actor owner가 bind 또는 rebind 성공 시 새 token을 발급한다. |
| 수명 | Rebind, unbind, session close 또는 generation 변경으로 무효화된다. 이전 token은 dispatch·reply·push·close에 사용할 수 없다. |

<a id="binding-route"></a>
### Binding route

Session owner가 특정 Actor binding에 보관하는 현재 Actor owner 전달 경로다. Bind가
성공하면 검증한 `ActorRef` 위치로 만든 route를 저장하고, relay·disconnect 통지와
Actor push는 이 저장된 route를 사용한다. Message마다 Location Store를 다시 조회해
route를 선택하지 않는다.

Actor relocation에서는 target에서 복원되고 owner·membership commit을 마친 같은
`ObjectGeneration`의 Actor만 route를 갱신한다. Target runtime은 이동한 Actor의 binding
route와 bound-session current Actor location snapshot을 target owner 및 target
MeshName·NodeRid로 함께 갱신하도록 Session owner에 요청한다. ActorId·ObjectGeneration은 유지하며,
새 incarnation은 명시적으로 다시 bind해야 한다. 같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의
route와 location snapshot, physical STREAM connection은 유지한다. Location Store와 Relocation Store는
binding route를 저장하거나 갱신하지 않는다.

<a id="binding-route-ack"></a>
### Session Actor 위치 갱신 응답

Session owner가 새 binding route와 relocation 뒤 current Actor location snapshot을 저장하고,
이전 owner generation의 늦은 packet과 push를 current binding에 적용하지 않도록 검증을 마쳤다는
`sessionActorLocationUpdateResMsg`다. Snapshot은 같은 ActorId·ObjectGeneration과 target
MeshName·NodeRid를 가진다. 이 응답은 위치 갱신 재전송을 중단하기 위해 사용하며 Target
Actor의 message 처리나 Join completion을 허용하는 신호가 아니다.

응답 결과는 `Applied`, `AlreadyApplied`, `Stale` 또는
`SessionOrBindingClosed`다. 앞의 두 결과는 요청한 위치가 적용됐음을 뜻한다. 뒤의 두
결과는 더 최신 위치가 있거나 Session·binding이 종료되어 이전 위치를 적용하지 않았음을 뜻한다.
정확한 wire 값과 Message Follow route 제거 조건은
[Session–Actor dispatch §5.1](20-session-actor-dispatch.ko.md#51-session-actor-위치-갱신-message)이
정의한다.

<a id="binding-generation"></a>
### Binding generation

같은 session owner process lifecycle 안에서 binding이 교체된 순서를 구분하는
owner-local 값이다. 다른 owner나 재시작한 process의 값과 크기를 비교하지 않는다.

| 항목 | 내용 |
|---|---|
| 형태 | Owner-local monotonic generation |
| .NET 표기 | Application에 직접 노출하지 않는 `ulong` contract value |
| 공개 구성 | 1 이상의 generation 값 하나다. Binding identity에서는 session owner node RID와 node lifecycle generation을 함께 사용한다. |
| 생성·관리 | Session owner가 bind·rebind 순서에 따라 증가시킨다. |
| 수명 | 같은 session owner process lifecycle 안에서만 비교한다. Owner나 process lifecycle이 다르면 값의 대소를 비교하지 않는다. |

<a id="authority-owner-generation"></a>
### AuthorityOwnerGeneration

같은 object incarnation에서 authority owner가 바뀐 순서를 나타내는
provider 발급 값이다. Object가 delete 뒤 다시 생성되었는지를 구분하는
ObjectGeneration과는 다른 값이다.

| 항목 | 내용 |
|---|---|
| 형태 | Provider-issued monotonic generation |
| .NET 표기 | `ulong AuthorityOwnerGeneration` |
| 공개 구성 | `1..long.MaxValue` 범위의 정수 하나다. |
| 생성·관리 | Location Store provider의 global durable counter가 initial reserve와 `NewOwner` transition에서 발급한다. |
| 수명 | 같은 ObjectGeneration 안에서 owner가 바뀔 때 새 값을 사용한다. 최대값에서 wrap하지 않고 `GenerationExhausted`로 실패한다. |

<a id="owner-lease-generation"></a>
### OwnerLeaseGeneration

현재 object owner가 속한 host process lifecycle을 구분하는 provider 발급 값이다.
같은 owner identity가 재시작해도 이전 process에서 온 작업을 current 작업으로
받지 않게 한다.

| 항목 | 내용 |
|---|---|
| 형태 | Provider-issued owner lifecycle generation |
| .NET 표기 | `long OwnerLeaseGeneration`; owner token에서는 `ZLinkLocationOwnerToken.LeaseGeneration` |
| 공개 구성 | Positive generation 값 하나이며 `OwnerId`와 함께 exact owner token을 만든다. |
| 생성·관리 | Location Store provider가 새 owner lease claim에서 발급한다. |
| 수명 | 해당 host process owner lease lifecycle 동안 유지된다. 같은 OwnerId가 재시작해도 새 값을 사용한다. |

<a id="session-sequence"></a>
### Session sequence

한 STREAM session에서 수락한 ingress message의 순서를 나타내는 값이다. Actor
handoff에서는 마지막으로 수락한 순서를 barrier로 사용하여 이전 owner와 새 owner가
같은 message를 함께 처리하지 않게 한다.

| 항목 | 내용 |
|---|---|
| 형태 | Per-session monotonic sequence |
| .NET 표기 | Application에 직접 노출하지 않는 `ulong` contract value |
| 공개 구성 | `1..long.MaxValue` 범위의 ingress 순서 값 하나다. Binding generation과 별개의 축이다. |
| 생성·관리 | Session owner가 ingress message를 수락하는 순서대로 증가시킨다. |
| 수명 | 한 STREAM session 안에서만 비교한다. Actor handoff barrier가 마지막 accepted sequence를 high-water로 고정한다. |

## 11. Stream Connector

<a id="stream-connector"></a>
### Stream Connector

서버 Framework의 STREAM 모델에 접속하여 packet을 주고받는 client library다. 서버
Framework package와는 별도로 배포하며 transport, codec, reconnect와 dispatch를
client 실행 환경에 맞게 제공한다.

| 항목 | 내용 |
|---|---|
| 형태 | Client runtime component |
| .NET 표기 | `IZlinkStreamConnector`, `ZlinkStreamConnectorFactory`, `ZlinkStreamConnectorOptions` |
| 공개 구성 | Connection lifecycle, typed send/request, wait·handler registration, pending dispatch queue와 runtime event를 제공한다. |
| 수명 | Factory 생성부터 `Close`와 `DisposeAsync` 완료까지 유지된다. |

<a id="stream-packet"></a>
### Stream packet

Message kind와 선택적인 packet name 같은 header 정보에 payload를 결합한 STREAM
전송 단위다. Request와 reply를 연결하는 값도 header에 포함한다.

| 항목 | 내용 |
|---|---|
| 형태 | Binary frame과 복합 header |
| .NET 표기 | Raw packet public type 없음. .NET Connector는 typed send/request call과 `ZLinkMessage` 계열 값으로 감싼다. |
| 생성·관리 | Connector runtime이 encode·decode하며 application은 header를 직접 조립하거나 수정하지 않는다. |
| 수명 | 한 STREAM transport frame의 송수신과 pending request matching 동안 유지된다. |

| Wire 구성 | 형식 |
|---|---|
| Frame prefix | `u16 header_len`, `u32 payload_size` |
| Fixed header | `format_marker = 0xF2`, `kind u8`, `codec u8`, `flags u8` |
| Request sequence | Flag가 있을 때 `request_seq u64`; `0`은 사용하지 않는다. |
| Packet name | `u8 name_len + UTF-8 bytes`, 최대 255 byte. Response와 Error는 길이 `0`이다. |
| Metadata | Flag가 있을 때 `u16 meta_len + encoded metadata` |
| Correlation ID | Flag가 있을 때 `u8 length + ASCII bytes` |
| Flow | Flag가 있을 때 36 byte `flow_id`와 1 byte `flow_origin`이 함께 존재한다. |
| Payload | Header 뒤의 `payload_size` byte |

모든 multi-byte 정수는 network byte order를 사용한다.

<a id="dispatch-mode"></a>
### Dispatch mode

수신 callback을 receive loop에서 자동으로 실행할지, application이 지정한 문맥에서
명시적으로 pump할지를 정하는 Connector 설정이다. 게임 엔진에서는 main thread
제약 때문에 기본값으로 `Manual`을 사용한다.

| 항목 | 내용 |
|---|---|
| 형태 | Closed Connector execution mode |
| .NET 표기 | 현재 공통 .NET exact interface에는 독립 public enum이 없다. C# contract pseudocode로는 `Manual`과 `Immediate` 두 값이다. |
| 공개 구성 | `Manual`은 dispatch queue를 application이 명시적으로 pump하고 `Immediate`는 receive 경로에서 callback을 inline 실행한다. |
| 생성·관리 | Application이 Connector option에 지정하며 게임 엔진의 기본값은 `Manual`이다. |
| 수명 | Connector instance configuration 동안 유지된다. |
