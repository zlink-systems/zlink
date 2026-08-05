---
title: "Actor 모델"
---

# Actor 모델

[스펙 목차](README.ko.md) · [이전: MeshNode](13-mesh-node.ko.md) · [다음: Spot과 Actor membership](15-spot-actor.ko.md)

> **이 장이 정의하는 것** — Actor의 identity, 위치, message queue, lifecycle과
> session binding.


## 1. 이 문서가 정의하는 범위

이 문서는 ZLink Framework에서 Actor의 identity, 위치, message queue,
lifecycle과 session binding을 정의한다.

Actor가 Entry Spot, User Spot 또는 remote MeshNode 중 어디에 있더라도 application
payload는 Actor 자신의 queue에 제출한다. Queue에 들어온 handler를 실행할 gate는
현재 Spot membership과 User Spot execution mode가 결정한다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| [MeshNode](01-glossary.ko.md#meshnode) route와 peer admission | [MeshNode](13-mesh-node.ko.md) |
| [Spot](01-glossary.ko.md#spot) [membership](01-glossary.ko.md#membership) transaction과 relocation | [Spot Actor](15-spot-actor.ko.md) |
| STREAM session 연동 | [Session Actor Dispatch](20-session-actor-dispatch.ko.md) |
| Payload와 metadata | [메시지 모델](04-message-model.ko.md) |
| Callback 실행과 completion | [비동기 실행 정책](05-async-execution-policy.ko.md) |

## 2. Actor identity와 서로 독립적인 상태

### 2.1 ActorId와 stable type

Actor는 Location Store namespace 전체에서 유일한 logical `ActorId`로 식별하는
stateful object다.

`ActorId`는 UTF-8 `1..255` bytes이며 대소문자를 구분하는 exact value다. Framework는
Unicode normalization이나 case folding을 적용하지 않는다.

`MeshName`은 Actor를 처음 배치할 곳을 선택할 때 사용하는 속성이며 Actor identity에
포함되지 않는다. 따라서 같은 `ActorId`를 서로 다른 Mesh마다 중복 생성할 수 없다.

Actor type은 UTF-8 `1..255` bytes의 stable name이다. Actor를 생성할 때 이 이름으로
factory를 선택한다. 언어의 class 이름이나 generic type 이름을 Store 또는 wire의
identity로 사용하지 않는다. 같은 server에 같은 stable type을 두 번 등록하면
startup 오류다.

### 2.2 ActorRef

`ActorRef`는 특정 시점의 Actor 위치를 나타내는 변경할 수 없는 snapshot이다.

| `ActorRef` field | 의미 |
|---|---|
| `ActorId` | Logical Actor identity다. |
| `ObjectGeneration` | 같은 ActorId의 서로 다른 logical incarnation을 구분하는 0이 아닌 unsigned 63-bit 값이다. Relocation 중 target에서 Actor 객체를 다시 만드는 `RecreateOnRelocation`은 같은 incarnation을 계속 사용하므로 이 값을 바꾸지 않는다. |
| 현재 `MeshName` | 현재 owner가 속한 Mesh다. |
| 현재 `NodeRid` | 현재 [owner](01-glossary.ko.md#owner) node의 RID다. |

`ActorRef`는 Actor message의 target으로 사용하는 값이 아니다. Actor가 이동하거나
다시 만들어지면 이전 `ActorRef`는 stale할 수 있다.

[`ObjectGeneration`](01-glossary.ko.md#objectgeneration)은 JSON에서 decimal string으로 표현한다. 별도의
`ActorRefSnapshot` public type은 제공하지 않는다.

### 2.3 Spot membership과 STREAM binding

Actor는 다음 두 상태를 서로 독립적으로 관리한다.

| 상태 축 | 가능한 상태 | 이 상태가 나타내는 것 |
|---|---|---|
| Spot membership | [Entry Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot), user Spot, 이동 중 | Actor의 logical 위치와 Spot membership을 나타낸다. |
| STREAM binding | unbound, bound | 현재 client session으로 push하거나 session payload를 받을 수 있는지를 나타낸다. |

Actor가 user Spot에 존재하기 위해 bound session이 필요하지 않다. Session bind나
unbind도 Actor의 현재 Spot을 바꾸지 않는다.

한 Actor는 동시에 session 하나에만 bind할 수 있다. 반대로 session 하나에는 여러
Actor를 bind할 수 있다.

## 3. Actor queue

모든 Actor application payload는 target Actor의 application queue에 직접 제출한다.
Actor가 Entry Spot이나 user Spot에 있거나 remote MeshNode에 있어도 같은 규칙을
적용한다.

- 같은 Actor queue가 수락한 payload는 Actor turn에서 순서대로 처리한다.
- Entry Spot Actor와 `PerActor` User Spot의 Actor는 Actor별 gate를 사용한다. 서로
  다른 Actor는 독립적으로 실행할 수 있다.
- `SpotWide` User Spot의 member Actor는 User Spot 공통 execution gate를 사용한다.
  같은 User Spot의 Actor·Spot handler·timer·lifecycle callback은 전체에서 한 번에
  하나만 실행한다.
- `Yield`는 `SpotWide` User Spot의 공통 gate에서 실행 중일 때만 사용할 수 있다.
  Entry Spot Actor와 `PerActor` User Spot의 Actor에는 제공하지 않는다.
- `SpotWide` member Actor가 `Yield`해도 현재 Actor queue head에 대한 claim은 유지한다.
  다른 Actor·Spot handler·timer는 반납된 User Spot gate를 사용할 수 있지만 같은 Actor의
  다음 payload는 현재 continuation이 끝날 때까지 시작하지 않는다.
- Actor send/request, [STREAM session](01-glossary.ko.md#stream-session) relay와 Actor 사이의 호출은 모두 같은 Actor
  queue로 들어간다.
- Actor payload를 Spot application queue에 넣거나 Spot callback으로 변환하지 않는다.

User Spot execution mode와 `Yield` continuation의 실행 규칙은
[비동기 실행 정책](05-async-execution-policy.ko.md#11-submit-async와-yield)이
정의한다.

### 3.1 Deferred Join barrier

Actor Join은 `JoinSpot(...)` 또는 `JoinEntrySpot(...)` call의 `Defer()`로 등록한다.
`Defer()`는 현재 handler가 끝난 뒤 Join을 실행하도록 예약하는 동기 terminal이다.
호출한 자리에서는 target을 찾거나 Store에 접근하지 않는다. 현재
handler에 변경할 수 없는 Join 요청을 기록하고, 이 Actor의 다음 message가 Join보다
먼저 실행되지 않도록 비활성 queue barrier만 등록한다.

Join call에는 `Async`, `await`, `submit`, coroutine terminal과 `Yield`를 제공하지
않는다. `Defer()` 자체도 Spot gate나 Actor queue claim을 반납하지 않는다. 현재
handler는 계속 실행하며, 마지막 awaited continuation까지 정상적으로 끝나야
Framework가 barrier를 활성화하고 Join을 시작한다. Handler가 exception이나
cancellation으로 끝나면 그 handler가 등록한 비활성 barrier를 모두 폐기한다.

Registration은 Actor exact generation, current membership, immutable request
snapshot, absolute deadline과 non-zero 128-bit operation ID를 고정한다.

한 handler는 Join을 최대 64개까지 등록할 수 있다. Join request 하나의 encoded
크기는 최대 1 MiB이며 같은 handler가 등록한 모든 Join request의 합계는 최대
8 MiB다. Request를 생략하면 empty `ZLinkMessage`를 고정한다. 각 `Defer()`는
request를 변경할 수 없는 snapshot으로 만들고 monotonic clock을 기준으로 absolute
deadline을 계산한다. Timeout 기본값은 5초이며, 명시한 값은 millisecond로 올림한
`1..INT_MAX` 범위의 유한한 값이어야 한다. 제한을 넘긴 현재 registration은 일부
record를 남기지 않고 동기 startup configuration error로 실패한다.

Cross-node Join의 application reply도 최대 1 MiB다. Request와 reply의 크기 제한은
서로 독립적이다. Crash recovery를 위해 둘을 저장할 때도 하나의 1 MiB 제한으로
합치지 않는다.

Actor send/request handler와 User·Entry Spot의 packet·request·subscription·timer handler에서 local member
Actor의 Join을 등록할 수 있다. Factory, `Configure`, lifecycle callback, relocation adapter, detached task,
Instance Spot handler와 Framework가 관리하지 않는 thread에서는 `InvalidOperation`이다. 같은 call의
두 번째 `Defer()`는 `InvalidOperation`, 같은 Actor의 다른 pending membership transition은 `Unavailable`이다.

Framework가 `Defer()`를 허용하는 시간 범위를 handler registration scope라 한다.
Handler가 실행되는 동안과 Framework가 추적하는 awaited continuation에서는 이
scope가 열려 있다. Scope가 닫힌 뒤 호출하면 `InvalidOperation`이다.
Application이 handler에서 시작하고 기다리지 않은 detached task에서 `Defer()`를
호출하는 것은 계약 위반이다. Framework는 모든 언어에서 이 오용을 handler 종료
전에 발견한다고 보장하지 않는다.

Handler turn, 비활성 barrier와 scope는 현재 process의 메모리에만 유지한다. Join
실행이나 Location Store commit 전에 process가 종료되면 이 registration과
completion을 재생하지 않으며 source authority와 membership을 그대로 유지한다.

Registration 뒤 source seal 전 도착한 payload는 barrier 뒤 Actor queue에 수락하고 cross-node relocation에서는
accepted journal·실행 전 queue와 함께 이관한다. Source seal 이후 CAS 전과 Message Follow 구간의 payload만
처리 방식은 서로 다르다. CAS 전 payload는 bounded ingress hold에 보관한다. CAS가 끝난 뒤 이전 owner에
도착한 payload는 [Message Follow](01-glossary.ko.md#message-follow)로 새 owner에 전달한다.

같은 handler가 barrier를 등록한 Actor에 request를 보내고 그 reply를 기다리면,
request는 barrier 뒤에서 기다리고 handler도 끝날 수 없어 순환 대기가 생긴다.
Framework는 이 request를 제출하기 전에 `InvalidOperation`으로 거부한다.

Join과 maintenance가 경쟁하면 먼저 확정한 제어 상태를 따른다. Join claim이
`Relocate`보다 먼저면 maintenance는 Join이 terminal 상태가 될 때까지 기다린다.
`Relocate` seal이 먼저면 Join은 `Unavailable`, shutdown admission seal이 먼저면
`ShuttingDown`으로 실패한다.

Actor가 이미 요청한 User Spot에 속해 있거나 Entry Spot Actor가 다시
`JoinEntrySpot`을 호출하면 실제 위치를 바꾸지 않고 `Accepted` completion을
실행한다. Location Store와 membership을 변경하지 않으며 join·joined·leave
lifecycle callback도 실행하지 않는다.

Request handler가 application reply를 encoding하지 못하면 handler failure로
처리하여 비활성 barrier를 폐기한다. Encoding이 끝난 뒤 caller가 연결을
종료했거나 transport가 reply를 수락하지 못한 경우에는 이미 등록한 Join을
취소하지 않는다.

Actor handler는 Actor 자신의 mutable state를 소유한다. Room, stage 또는 zone처럼
Spot이 소유한 상태를 읽거나 바꾸려면 Actor handler가 명시적인 Spot send/request를
제출해야 한다. 이 작업은 target Spot turn에서 실행된다.

Actor handler는 containing Spot object를 받는다. `SpotWide`에서는 shared gate 안에서 Spot state를 사용할
수 있다. `PerActor`와 Entry에서는 containing Spot의 mutable state를 직접 공유하지 않고 위의 명시적인
Spot send/request를 사용한다.

Actor가 Ready가 되었다는 notification, request 완료, relocation 단계 전환과 session
binding 진행은 Framework가 전용 queue에서 처리한다. Actor의 업무 handler가
실행되는 queue와 분리되어 있으므로 application handler가 비동기 작업을 기다리는
동안에도 계속 처리할 수 있어야 한다.

## 4. Spot이 처리하는 Actor control

Spot은 Actor application payload를 처리하지 않는다. [Spot turn](01-glossary.ko.md#spot-turn)에서 처리하는 Actor
관련 작업은 membership과 lifecycle control뿐이다.

| Control 작업 | Spot에서 처리하는 내용 |
|---|---|
| Join | Actor membership을 허용할지 판단하고 Spot이 소유한 membership을 갱신한다. |
| Leave | Membership을 해제하고 Spot이 소유한 상태를 정리한다. |
| Relocation prepare·commit·abort | 이동 transaction에 맞춰 Spot이 소유한 상태를 일관되게 변경한다. |
| Actor lifecycle notification | Actor 생성·종료 뒤 Spot이 소유한 후속 작업을 실행한다. |

Framework는 이 lifecycle 작업을 target Spot의 전용 queue에 넣는다. 같은 Spot의
다른 callback과 하나씩 실행하므로 두 callback이 Spot 상태를 동시에 바꾸지 않는다.

Actor가 소유한 상태를 바꾸는 lifecycle 작업도 Actor의 전용 queue에서 하나씩
실행한다. Actor와 Spot 양쪽 상태를 함께 바꾸는 순서와 오래된 owner의 변경을
거부하는 규칙은 [Spot Actor](15-spot-actor.ko.md)가 정의한다.

Lifecycle queue와 application payload queue가 함께 실행 가능하면 **lifecycle queue를
먼저 실행한다.** Join이 끝나기 전에 그 Actor 앞으로 온 payload를 실행하거나, leave가
확정된 뒤에 payload를 실행하는 것을 막기 위해서다. 이 우선순위는 두 queue 사이에만
적용하며 각 queue 안의 수락 순서는 바꾸지 않는다.

이 우선순위는 **절대 우선순위가 아니다.** 여기에는 서로 다른 두 상한이 관여하며 섞으면
안 된다.

| 상한 | 무엇 사이의 공정성인가 | 어디서 정의하는가 |
|---|---|---|
| owner 점유 상한 | **서로 다른 owner** 사이 | [Framework API](06-framework-api.ko.md) |
| lifecycle 연속 실행 상한 | **같은 owner 안의 두 lane** 사이 | 이 절 |

owner 점유 상한에 도달하면 그 owner 전체가 turn을 놓고 다른 ready owner가 실행한다.
이것만으로는 lane 사이의 굶주림을 막지 못한다 — 이 owner에 turn이 돌아왔을 때 두 lane이
여전히 ready이면 같은 우선순위 규칙이 lifecycle을 다시 고르기 때문이다.

그래서 lifecycle lane에 **연속 실행 상한**과 **양보 부채**를 따로 둔다.

연속 실행 상한은 **lifecycle lane을 연속으로 고른 turn 수**로 센다. 시간이 아니라 turn
수인 이유는, 실행 시간 상한은 이미 owner 점유 상한이 담당하고 있고 lane 사이의 문제는
"몇 번 연속으로 고르는가"이기 때문이다.

1. Lifecycle lane을 고를 때마다 연속 횟수를 하나 올린다.
2. 연속 횟수가 상한에 도달하면 그 owner에 **양보 부채**를 표시하고 횟수를 0으로 되돌린다.
3. 양보 부채가 있는 owner는 application lane이 ready인 한, 이 owner가 turn을 얻을 때
   **application lane을 먼저 실행한다.**
4. Application turn을 한 번 실행하면 부채를 지운다.

경계 조건은 다음과 같다.

| 상황 | 처리 |
|---|---|
| Lifecycle lane이 비어 application lane을 골랐다 | 연속 횟수를 0으로 되돌린다 |
| 부채가 있는데 application lane이 ready가 아니다 | 부채를 유지한 채 lifecycle lane을 계속 실행한다. application 작업이 없으면 굶주림도 없다 |
| 다른 owner에게 양보했다가 돌아왔다 | 부채와 연속 횟수를 그대로 유지한다. owner 점유 상한과는 별개다 |
| Owner가 종료하거나 이동한다 | 부채와 연속 횟수를 함께 버린다 |

**부채가 어디에 붙는지는 execution mode에 따라 다르다**
([Spot 메시징 §5.4](12-spot-messaging.ko.md)).

| Mode | 부채가 붙는 자리 | 무엇이 부채를 해소하는가 |
|---|---|---|
| `SpotWide` User Spot, Entry Spot, Instance Spot | 공유 execution gate 하나 | 그 gate에서 실행하는 application 작업 아무거나 |
| `PerActor` User Spot | gate마다 따로 — Actor gate, Spot lane gate, timer gate | 그 gate의 application 작업. Actor lifecycle 부채는 **그 Actor의** application 작업만, Spot lifecycle 부채는 **Spot lane의** application 작업만 해소한다 |

`PerActor`에서 부채를 gate 단위로 두지 않으면 한 Actor의 lifecycle 폭주가 다른 Actor의
turn으로 해소된 것처럼 계산되어 그 Actor의 application 작업이 계속 밀린다.

이 보장은 아직 **정성적이다** — owner 점유 상한에 값과 허용 범위가 정해져 있지 않으므로,
"몇 ms 안에 실행된다"를 이 조항으로 판정할 수 없다. 값이 정해지기 전까지 검증할 수 있는
것은 "lifecycle 작업이 계속 도착해도 application turn이 실행되기는 한다"까지다.

## 5. Actor 메시징

Actor send/request의 target은 global `ActorId`다. Framework는 [Ready](01-glossary.ko.md#ready) 상태인 현재
incarnation과 [authority](01-glossary.ko.md#authority)가 가리키는 owner route를
positive route cache 또는 [Location Store](01-glossary.ko.md#location-store)에서 찾는다.
그 뒤 owner fence를 확인하고 target queue에 message를 제출한다. Resolve할 때 확인한
`ObjectGeneration`은 route snapshot과 stale cache를 구분하는 정보이며 Actor handler의 target
일치 조건이 아니다.

Local Actor와 remote Actor는 handler 실행과 completion에 같은 의미를 사용한다.

Caller는 다음 값을 Actor message target으로 지정하지 않는다.

- `MeshName`
- `ActorRef`
- Owner RID
- 현재 Spot ID

### 5.1 Route cache와 generation

- Missing, Creating과 Store failure 결과는 negative cache에 저장하지 않는다.
- Ready 상태인 현재 위치를 보관하는 positive cache도 current owner lease의 local
  admission deadline과 공개 `RouteCacheMaxAge` 안에서만 사용한다.
- 더 큰 StoreVersion, stale result 또는 Store recovery event를 확인하면 positive
  cache를 즉시 무효화한다.
- `ObjectGeneration`은 Actor direct message의 target 일치 조건이 아니다.
- Resolve 뒤 같은 owner에서 Actor가 destroy되고 같은 `ActorId`로 다시 만들어졌다면,
  target queue가 수락하는 시점의 current Ready Actor가 message를 처리한다.
- Resolve한 owner가 더 이상 해당 ActorId를 소유하지 않으면 현재 operation은 stale route
  오류로 끝낸다. Framework는 Location Store에서 새 owner를 찾아 같은 operation을 자동으로
  다시 보내지 않는다.
- Request timeout이나 실행 여부를 알 수 없는 실패가 발생해도 Framework가 자동으로
  재전송하지 않는다.
- Actor direct messaging은 session binding을 만들거나 바꾸지 않는다.

### 5.2 Handler 선택

Framework는 Actor type, message kind와 packet name으로 handler를 선택한다. 같은
Actor handler namespace에 같은 key를 두 번 등록하면 startup 오류다.

Handler type과 signature는 언어별 공개 interface 문서가 정의한다.
Handler instance와 scoped dependency는 hosting Spot이 아니라 해당 Actor activation이
소유한다. 서로 다른 Actor가 같은 handler instance를 공유하지 않으며 relocation과
cross-node Join 뒤 target Actor activation에서 다시 만든다. 자세한 수명 계약은
[Framework API의 handler 수명](06-framework-api.ko.md#82-handler-실행-객체와-dependency-수명)을
따른다.

Actor와 Actor Context는 composition 관계다. Framework는 factory를 호출하기 전에 `ActorId`,
`ObjectGeneration`, current `MeshName`, nullable current `SpotId`와 bound-session capability를 가진 exact
Context를 만든다. Factory는 ID를 별도 인자로 받지 않고 이 Context만 받는다. 반환한 Actor는 전달받은
Context를 read-only `Context` member로 그대로 노출해야 하며 `Configure()`는 Context 인자를 받지 않는다.
다른 Context를 반환하면 staging Actor를 Ready로 공개하지 않는다.

Same-node Join은 Actor instance와 Context를 유지하고 membership commit에서 `SpotId`만 바꾼다. Cross-node
Join은 Actor ID와 ObjectGeneration을 유지하되 target owner와 membership에 결합한 새 Context를 target
factory에 전달한다. Commit 뒤 source Context의 identity는 source leave callback까지 읽을 수 있지만 새
send/request/session mutation/Join은 `Unavailable`로 끝나며 current target으로 자동 전달하지
않는다.

## 6. Actor lifecycle

### 6.1 Factory와 relocation policy 등록

Object Server는 다음 값을 함께 등록한다.

- Actor [stable type](01-glossary.ko.md#stable-type)
- [Factory](01-glossary.ko.md#factory)
- factory configure callback에서 선택하는 relocation policy

Relocation policy를 생략하는 overload나 compatibility default는 제공하지 않는다.

`PreserveStateWith`를 선택하면 해당 Actor type에 맞는 `ActorRelocationAdapter`를 같은
등록에서 제공해야 한다. Adapter는 Actor 상태를 application만 해석하는 byte sequence로
저장하고 복원한다. Framework는 이 byte sequence의 내용을 해석하지 않으며 별도의
state contract ID도 관리하지 않는다.

`PreserveStateWith`는 source handler가 정상적으로 끝난 시점의 application state를
capture하여 target Actor에 복원한다. `RecreateOnRelocation`은 target에서 Actor 객체를 다시
만들지만 application state를 복원하지 않는다. 대신 Framework가 소유한 실행 전
queue와 timer 정보는 이동 후에도 유지한다. 두 policy 모두 같은 logical Actor의
이동이므로 `ObjectGeneration`을 바꾸지 않는다. Cross-node 이동에서 owner가 바뀌면
`AuthorityOwnerGeneration`만 증가한다.

이동하는 Actor가 Session에 bind되어 있으면 target에서 Actor를 복원하고 owner·membership을
commit한 뒤 Actor message 처리를 시작한다. 그 뒤 target runtime이
`sessionActorLocationUpdateReqMsg`를 send하여 Session owner에 저장된 해당 Actor binding
route를 target owner로 갱신하도록 요청한다. 여기서 binding route는 Session owner가 현재
Actor owner에 message를 보낼 때 사용하는 전달 경로다. Route switch와 함께 bound-session
accessor가 반환하는 current Actor location snapshot도 같은 ActorId·ObjectGeneration을
유지한 채 target MeshName·NodeRid로 갱신한다. 같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의 route와
physical STREAM connection은 유지한다. Session owner는 갱신한 뒤
`sessionActorLocationUpdateResMsg`를 send한다. 응답이 없으면 target runtime은 최초 send
1초 뒤부터 1초, 2초, 4초, 5초 간격으로 같은 요청을 다시 보내고 이후에는 5초 간격을
유지한다. 응답을 기다리는 동안에도 Target Actor는 message를 처리한다. Route update는 같은
ObjectGeneration의 relocation에만 허용하며 application은 relocation을 알기 위해 rebind하지
않는다. 새 incarnation은 explicit bind가 필요하다.

다음 .NET 발췌는 factory와 relocation policy를 함께 등록하는 공통 규칙을 이해하기
위한 예시다. 다른 언어에 같은 signature를 요구하지 않으며, 정확한 .NET 계약은
[.NET Actor interface](server/languages/dotnet/interfaces/06-actors.ko.md)가
정의한다.

```csharp
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

public interface IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    IZLinkActorFactoryBuilder<TActor> DisableRelocation();
    IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation();
    IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>;
}
```

### 6.2 Create와 GetOrCreate 입력

Actor manager의 `Create`와 `GetOrCreate`는 한 번만 제출할 수 있는 fluent call이다.
두 call 모두 다음 required 값을 받는다.

- `ActorId`
- Stable Actor type

다음 값은 선택 사항이다.

- `InMesh`
- Encoded creation request
- Timeout

Caller는 target RID, predicate, factory class 또는 placement callback을 지정할 수
없다.

같은 option을 두 번 설정하면 `InvalidOperation`이다. Terminal submit을 두 번
실행하면 `InvalidOperation`이다.

Terminal submit을 시작할 때 end-to-end [deadline](01-glossary.ko.md#deadline) 하나를 고정한다. 이 deadline은
resolve, reservation, factory 실행과 Ready barrier 전체에 적용된다.

다음 .NET 발췌는 fluent call에서 required 값과 optional 값을 나누는 방법을 보여준다.

```csharp
public abstract record ZLinkActorCreateResult
{
    public sealed record Existing(ActorRef Actor)
        : ZLinkActorCreateResult;

    public sealed record Created(
        ActorRef Actor,
        ZLinkMessage? Reply)
        : ZLinkActorCreateResult;

    public sealed record Rejected(ZLinkMessage? Reply)
        : ZLinkActorCreateResult;
}

public interface IZLinkActorManager
{
    IZLinkActorCreateCall Create(string actorId, string actorType);
    IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType);
    ValueTask<ActorRef?> FindAsync(
        string actorId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> DestroyAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorCreateCall
{
    // Actor를 처음 생성할 Mesh를 지정한다.
    // Object role Mesh가 하나면 생략할 수 있고, 둘 이상인데 생략하면 InvalidOperation이다.
    IZLinkActorCreateCall InMesh(string meshName);
    IZLinkActorCreateCall Request(ZLinkMessage request);
    IZLinkActorCreateCall Request<TRequest>(TRequest request);
    IZLinkActorCreateCall Timeout(TimeSpan timeout); // 전체 생성 deadline을 정한다.
    ValueTask<ZLinkActorCreateResult> Async(
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
}
```

```csharp
ZLinkActorCreateResult result = await actorManager
    .Create("player-42", "player") // ActorId와 stable type은 required 값이다.
    .InMesh("world")
    .Timeout(TimeSpan.FromSeconds(5))
    .Async(cancellationToken);     // Created 또는 Rejected를 반환한다.
```

### 6.3 Mesh와 placement target 선택

`InMesh`를 지정했다면 해당 Mesh를 사용한다. 생략했다면 다음 규칙으로 Mesh를 정한다.

| 조건 | 결과 |
|---|---|
| Object `Client` 또는 `Server` role을 가진 Mesh가 하나다. | 그 Mesh를 자동 선택한다. |
| 후보가 없다. | `NotConfigured`로 끝난다. |
| 후보가 둘 이상이다. | `InvalidOperation`으로 끝난다. |
| `InMesh`로 지정한 Mesh가 없다. | `NotFound`로 끝난다. |

Framework는 다음 순서로 target 후보를 검사한다.

1. Object role을 확인한다.
2. 요청한 stable type이 등록되어 있는지 확인한다.
3. Active·pending capacity가 남아 있는지 확인한다.
4. 남은 후보에 node-wide placement weight를 적용한다.

Caller는 target node나 endpoint를 선택하지 않는다.

### 6.4 Creation request와 factory 실행

Encoded creation request는 최대 1 MiB다. Framework는 reservation을 시작하기 전에
내용이 바뀌지 않는 creation request reference와 hash를 durable creation intent에
기록한다. 이 정보는 Actor가 Ready가 되거나 fence가 적용된 실패 정리를 마칠 때까지
유지한다.

여러 node가 같은 Actor를 동시에 생성하려고 해도 authority CAS에서 이긴 node만
creation request를 factory에 전달한다.

Factory는 같은 `(ActorId, ObjectGeneration, [creation attempt](01-glossary.ko.md#creation-attempt))`에 대해 한 번 이상
실행될 수 있다. 따라서 factory는 같은 attempt가 다시 실행되어도 안전해야 한다.

Factory가 만든 Actor는 아직 외부에 공개되지 않은 staging instance다. Entry Spot의
`OnCreateActor`가 creation request를 확인하고 `Accepted` 또는 `Rejected`와 optional
application reply를 반환한다.

- `Accepted`이면 initial Entry membership과 Ready authority를 commit하고 `Created`를
  publish한다. 이 최초 생성 과정에서는 `OnActorJoin`과 joined notification을 호출하지
  않는다.
- `Rejected`이면 Ready authority와 message admission을 만들지 않는다. Staging Actor,
  Creating authority와 reserved capacity를 정리하고 `Rejected`를 publish한다.
- Callback exception은 application이 선택한 `Rejected`가 아니라 typed creation
  failure로 처리하고 attempt를 `Aborted`로 끝낸다.

Actor를 Ready로 공개한 뒤 destroy하는 방식으로 거절을 구현하지 않는다. 거절된
Actor는 `Find`로 조회할 수 없고 message를 받을 수 없으며 active capacity를
소비하지 않는다.

### 6.5 Create와 GetOrCreate의 차이

Exclusive `Create`를 실행할 때 같은 type의 Ready Actor가 이미 있으면
`AlreadyExists`로 끝난다. 다른 type의 Actor가 있으면 `TypeMismatch`로
끝난다.

`GetOrCreate`는 같은 type의 Ready Actor가 있으면 새 reservation과 callback 실행 없이
현재 incarnation을 `Existing`으로 반환한다. 같은 type의 Actor가 Creating이면 그
상태가 끝날 때까지 bounded backoff로 authority를 다시 확인한다. 현재 attempt가
Ready로 끝나면 `Existing`을 반환하고, 거절 또는 실패 정리로 Missing이 되면 남은
deadline 안에서 새 reservation을 경쟁한다.

동일한 ActorId에 여러 process가 동시에 `GetOrCreate`를 호출해도 Location Store의
[reservation](01-glossary.ko.md#reservation-id) CAS에 성공한 caller만 생성 실행을
소유한다. 서로 다른 operation은 앞선 attempt의 terminal state나 application reply를
공유하지 않는다. 앞선 attempt가 `Rejected` 또는 실패로 끝나면 다음 reservation
winner가 자신의 creation request로 factory와 callback을 실행한다.

```text
Missing
  → Reserved(R1)
      ├─ Created(R1, ActorRef, ReplyRef?)
      ├─ Rejected(R1, ReplyRef?)
      └─ Failed(R1, Failure)

Creating(R1) observed by operation B
  → Ready: Existing
  → Missing after cleanup: Reserve(R2) and run B callback
```

각 상태의 의미는 다음과 같다.

| 상태 | 의미 |
|---|---|
| `Created` | Callback이 생성을 승인했고 Actor와 initial Entry membership이 Ready로 commit됐다. |
| `Rejected` | Callback이 정상적으로 생성 요청을 거절했다. Ready authority와 active capacity는 만들지 않는다. |
| `Failed` | Node 종료, timeout 또는 callback exception으로 정상적인 application 결과를 만들지 못했다. |
| `Existing` | 이미 Ready인 Actor를 조회한 결과다. 새 reservation과 callback 실행이 없다. |

Location Store는 `(source Node RID, source lifecycle generation, OperationId)`로
식별한 operation terminal record를 원래 deadline 뒤 5분까지 유지한다. 같은
operation의 중복 전달만 이 record를 읽어 이전 결과를 재사용한다. 새 operation은
retained terminal record를 읽지 않고 current authority를 기준으로 다시 판단한다.

Terminal record에는 request correlation이나 reply route가 없는
`creation-operation-terminal-v1` semantic envelope와 SHA-256을 저장한다. 같은
operation을 재처리할 때 Framework는 현재 request의 correlation과 reply route로
새 command reply를 encode한다. Envelope에는 `Created`·`Rejected`·failure 결과와
optional application reply를 포함하며 encoded size는 최대 1 MiB다. Actor 생성
terminal을 보존하기 위해 Relocation Store를 사용하지 않는다.

Creating 상태를 재확인하던 caller가 deadline에 도달하면 해당 caller는
`DeadlineExceeded`로 끝나지만
생성 attempt가 실패했다고 간주하지 않는다. 다음 call은 Location Store의 current
authority와 retained terminal record를 다시 확인한다.

### 6.6 Find

Manager의 `Find(ActorId)`는 Ready 상태인 current authority의 `ActorRef`를 반환한다.
Actor 생성을 시작하지 않으며 별도의 Actor directory도 제공하지 않는다.

### 6.7 Spot 이동

Actor를 user Spot으로 옮기는 join·leave·relocation은
[Spot Actor](15-spot-actor.ko.md)의 fencing과 barrier를 따른다.

이동 중에 수락한 payload를 이전 Spot callback으로 보내지 않는다. Payload는 Actor
queue에서 순서를 유지한다.

### 6.8 종료와 destroy

Actor 종료는 새로운 payload admission을 닫고 session binding과 location ownership을
정리한다. Bound session의 연결이 종료되었다는 이유만으로 Actor를 자동 종료하거나
현재 Spot에서 자동 leave하지 않는다.

Lifecycle 종료를 허용하는 정확한 상태와 transaction은
[Spot Actor](15-spot-actor.ko.md)가 정의한다.

Actor destroy는 exact `ActorRef`를 받는다. Actor가 user Spot에 있으면 먼저 leave
또는 Entry Spot join을 완료해야 한다.

Destroy는 membership 이동이 아니다. 따라서 성공 과정에서 `OnLeaveActor`를 다시
호출하지 않는다.

Framework는 다음 순서로 destroy를 진행한다.

1. 새로운 payload admission을 닫는다.
2. 진행 중인 lifecycle 작업을 정리한다.
3. Session binding을 제거한다.
4. Location ownership과 registry entry를 제거한다.

| 상태 | Destroy 결과 |
|---|---|
| 같은 incarnation이 이미 없다. | Idempotent `false`를 반환한다. |
| 같은 ActorId의 다른 generation이 있다. | `InvalidOperation`으로 끝난다. |
| Actor가 이동을 위한 seal 상태다. | `Unavailable`로 끝난다. |

Framework는 current `ActorRef`를 다시 찾아 새 incarnation을 종료하지 않는다.

## 7. Session binding

Session binding은 Actor와 현재 STREAM session 사이의 runtime 관계다. Binding token은
재연결과 늦게 도착한 이전 session 작업을 구분한다.

Actor handler는 현재 bound session을 사용해 다음 작업을 할 수 있다.

- Client로 one-way push 전송
- Session 연결 종료 요청

Session에서 들어와 Actor로 향하는 payload도 Actor queue에 직접 제출한다. Spot
membership은 route와 lifecycle 검증에 사용할 수 있지만 payload를 Spot callback으로
보내는 근거로 사용하지 않는다.

Bind, rebind, disconnect와 request correlation은
[Session Actor Dispatch](20-session-actor-dispatch.ko.md)가 정의한다.

## 8. 실패와 관측

### 8.1 실패

| 조건 | 결과 |
|---|---|
| Logical ActorId에 Ready authority가 없다. | Actor target 오류로 끝난다. |
| Exact-ref operation에서 mapping이 없다. | `Unavailable`로 끝난다. |
| Exact-ref의 generation이 current generation과 다르다. | `InvalidOperation`으로 끝난다. |
| Actor가 commit 전 seal 상태다. | `Unavailable`로 끝난다. |
| Bound session이 필요한 작업에 유효한 binding이 없다. | `InvalidOperation`으로 끝난다. Binding을 먼저 만들어야 하는 순서 문제다. |

Handler가 없거나 decode가 실패하거나 application handler가 예외를 반환하면 request는
복원 가능한 reply route로 오류를 반환한다. One-way message는 runtime 관측 경로에
오류를 기록한다.

Drain 중에는 새로운 Actor 생성과 membership 배정을 막는다. 이미 수락한 Actor turn과
control transaction은 deadline까지 진행한다.

### 8.2 관측 정보

Runtime은 다음 정보를 서로 구분하여 관측할 수 있어야 한다.

- Current `MeshName`과 Actor type
- Application queue와 control backlog
- ObjectGeneration
- Membership state
- Session-binding state
- Dispatch 결과

ActorId는 metric label로 사용하지 않는다.

## 9. 구현 및 contract test 검증 요구

- Entry Spot과 user Spot의 Actor payload가 모두 Actor queue로 직접 전달된다.
- Actor payload가 Spot callback이나 [Spot application queue](01-glossary.ko.md#spot-application-queue)를 거치지 않는다.
- Spot의 lifecycle 전용 queue에는 join·leave·relocation과 lifecycle control만
  넣으며 Actor 업무 payload를 넣지 않는다.
- Inbound dispatch가 Actor application instance를 찾기 전에 현재 relocation temporary queue가
  등록되어 있는지 확인한다. 있으면 해당 queue에 넣고, 없으면 기존 Actor dispatch를 사용한다.
- Restore 중 도착한 message를 temporary queue에서 실행하지 않는다. Commit과 lifecycle 작업이
  끝난 뒤 저장된 기존 작업을 실제 Actor queue에 먼저 넣고 temporary 작업을 그 뒤에 옮긴다.
- Temporary queue 제거와 기존 dispatch 전환을 atomic하게 처리하여 message가 중복되거나
  누락되지 않게 한다.
- Relocation Restore가 commit 전에 실패하면 target temporary queue를 실행하지 않고 폐기하며
  source가 소유한 원본을 되돌린다.
- 같은 `RelocationId`, target attempt와 owner generation의 Restore를 여러 번 받아도 temporary
  queue와 application instance를 한 번만 만든다. 이전 attempt의 temporary queue는 사용하지 않는다.
- 같은 Actor의 payload가 ingress 종류와 관계없이 Actor queue 수락 순서대로 실행된다.
- `SpotWide` member Actor가 `Yield`하면 User Spot gate만 반납하고 Actor queue claim을 유지한다. 이 동안
  다른 Actor·Spot·timer는 진행하지만 같은 Actor의 다음 job은 진행하지 않는다.
- 같은 Actor 자신에게 보낸 request가 `Yield` 뒤에도 현재 job을 앞질러 실행하거나 inline으로 재진입하지
  않는다.
- Actor Join은 `Async`와 `Yield`를 제공하지 않고 handler 안에서 동기 `Defer()`로
  등록한다. 결과는 Actor completion callback으로 전달한다.
- `Defer()`는 target 조회나 Store I/O 없이 Join intent와
  [비활성 barrier](01-glossary.ko.md#deferred-join-barrier)만
  등록하며 handler가 정상적으로 끝난 뒤에만 Join을 실행한다.
- Handler당 최대 64개, request 하나당 최대 1 MiB, request 합계 최대 8 MiB와
  기본 5초 timeout을 적용한다.
- Same-node Join, cross-node Join과
  [`RecreateOnRelocation` relocation policy](01-glossary.ko.md#relocation-policy)에서 같은 logical
  incarnation의 `ObjectGeneration`을 유지한다.
- Actor handler가 mutable Spot state에 직접 접근하지 않고 명시적인 Spot 호출을 사용한다.
- Session bind와 Spot membership이 독립적으로 바뀌며 서로를 암묵적으로 변경하지 않는다.
- 같은 ActorId를 서로 다른 MeshName에 중복 생성하지 않는다.
- Actor messaging이 ActorId만 받고 [owner route](01-glossary.ko.md#owner-route)와 generation을 application에 요구하지 않는다.
- 동시에 같은 Actor 생성을 요청해도 생성 권한을 얻지 못한 target은 factory를
  추가로 실행하지 않고 같은 attempt의 완료를 기다린다.
- Creating을 관찰한 서로 다른 operation은 Ready 뒤 `Existing`을 받고, rejection·failure
  cleanup 뒤 새 reservation을 경쟁하며 앞선 application reply를 공유하지 않는다.
- 같은 source Node RID·lifecycle generation·`OperationId`의 재전송만 correlation-free
  semantic terminal envelope를 읽고 현재 correlation·reply route로 reply를 다시 encode한다.
- `Rejected`와 `Aborted`가 Ready authority와 active capacity를 만들지 않고 reserved
  capacity를 반환한다.
- Terminal record가 original deadline 뒤 5분 동안 같은 operation의 replay를 허용하고,
  TTL 뒤 Ready authority가 없으면 새 reservation으로 다시 생성할 수 있다.
- Destroy가 exact generation을 검사하고 새 incarnation으로 retarget하지 않는다.
