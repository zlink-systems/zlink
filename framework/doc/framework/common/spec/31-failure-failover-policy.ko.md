---
title: "장애 대응과 failover 범위"
---

# 장애 대응과 failover 범위

[스펙 목차](README.ko.md) · [이전: Transport 연결 상태 확인](29-transport-liveness.ko.md) · [다음: Framework 오류 모델](32-framework-error-model.ko.md)

> **이 장이 정의하는 것** — connection·message operation·object 생성과 relocation
> 도중 장애가 났을 때 Framework가 같은 작업을 자동으로 계속하는 범위.


## 1. 이 문서가 답하는 질문

이 문서는 connection, message operation, object 생성과 relocation 도중 장애가 발생했을 때
Framework가 같은 작업을 자동으로 계속하는 범위를 정의한다. Application이 별도의
`FailoverPolicy`를 선택하는 public API는 없다. Framework는 operation 종류와 장애가 발생한
시점에 따라 이 문서의 고정된 규칙을 적용한다.

Framework는 처리 대상을 선택했는지만으로 operation 실행이 확정됐다고 판단하지 않는다.
Transport 또는 target queue가 operation을 수락하기 전에는 실행되지 않았음을 확인할 수
있으므로 허용된 다른 대상을 선택할 수 있다. 수락 여부가 불분명하거나 이미 수락된 뒤에는
중복 실행을 막기 위해 같은 operation을 다른 대상에 자동으로 제출하지 않는다.

이 문서에서 failover는 장애가 발생한 처리 대상을 다른 대상으로 바꾸고 application 작업을
계속하는 동작을 뜻한다. 끊어진 물리 connection을 같은 논리 peer에 다시 설정하는 reconnect,
계획된 점검 전에 stateful workload를 옮기는 `Relocate`, 다음 application 호출이 현재 상태를
다시 조회하는 동작은 failover와 구분한다.

## 2. 공통 판단 기준

Framework는 다음 순서로 자동 재선택이나 재실행 가능 여부를 판단한다.

Actor나 Spot을 현재 처리하는 node를 [owner](01-glossary.ko.md#owner)라고 한다. Owner를
사용하는 operation은 target node의 물리 connection만 보지 않고 Location Store의 object
generation과 owner 자격도 함께 확인한다.

1. Caller가 node RID처럼 target identity를 직접 지정했는지, Framework가 Channel server를
   선택하는 operation인지 확인한다.
2. Transport 또는 target queue가 operation을 수락했는지 확인한다.
3. Object operation이면 Location Store에서 확인한 owner와 generation이 아직 유효한지
   확인한다.
4. Stateful relocation이면 owner 변경 commit 전인지 후인지 확인한다.
5. 같은 operation을 계속할 수 없으면 한 번의 terminal 결과로 끝낸다. 다음 호출을 시작할지는
   Application이 결정한다.

| 확인한 경계 | Framework의 처리 |
|---|---|
| Framework가 target을 선택하며 아직 어느 target도 operation을 수락하지 않음 | 같은 operation의 deadline 안에서 다른 eligible target을 선택할 수 있다. |
| Caller가 node RID, global object ID 또는 Session binding을 지정함 | 지정한 logical identity를 유지한다. 다른 logical target으로 바꾸지 않는다. |
| Operation이 target queue에 수락됨 | 같은 operation을 다른 target에서 다시 실행하지 않는다. |
| Transport 수락 여부를 확인할 수 없음 | 중복 가능성이 있으므로 다른 peer에 자동 재제출하지 않는다. |
| Operation이 terminal 결과에 도달함 | Reply, failure, timeout, cancellation 또는 shutdown 가운데 먼저 확정된 결과 하나만 반환한다. |

Application은 실패 뒤 새 operation을 시작할 수 있다. 새 operation이 앞선 작업과 같은 변경을
다시 요청한다면 application protocol이 idempotency key나 현재 상태 확인으로 중복 영향을
막아야 한다. Framework는 실행 여부가 불분명한 앞선 operation을 실행되지 않은 것으로 바꾸지
않는다.

## 3. Channel target과 connection 장애

### 3.1 Channel server 재선택

Framework가 같은 ChannelName의 현재 server 중 하나를 고르는 동작을
[select-one](01-glossary.ko.md#select-one)이라고 한다. RouteMesh Channel은 새 operation을 받을 수 있는
[Ready](01-glossary.ko.md#ready) 상태이고 weight가
0보다 큰 server를 후보로 사용하고, ClientServer Channel은 `Ready`인 server를 후보로 사용한다.

첫 target의 non-blocking submit이 capacity 부족으로 수락되지 않으면 transport queue가
operation을 수락할 때까지 다른 eligible server를 선택할 수 있다. 수락된 뒤에는 reply가
없거나 connection이 끊어져도 다른 server에서 같은 operation을 다시 실행하지 않는다.

Node direct는 caller가 node RID를 지정하므로 이 재선택 규칙을 사용하지 않는다. 지정한 node가
없거나 connection이 준비되지 않으면 `NotFound` 또는 `Unavailable`로 끝난다. 자세한 선택과
완료 결과는 [상호작용 모델 §3](03-interaction-model.ko.md#3-node-direct와-channel-select-one)과
[Framework API §13](06-framework-api.ko.md#13-오류-kind)이 정의한다.

### 3.2 Connection 격리와 reconnect

Framework는 orderly close와 transport 오류를 즉시 반영하고, 응답이 없는 half-open
connection을 liveness deadline 안에 `not-ready`로 바꾼다. 한 peer의 장애는 다른 ready peer와
local owner의 처리를 중단시키거나 host 전체를 `Error`로 바꾸지 않는다.

Framework는 현재 configuration 또는 discovery descriptor를 사용해 같은 논리 peer와 connection을
다시 설정한다. 이때 service handshake와 identity 확인을 다시 수행한다. 이전 connection ID,
reply route, Session binding과 ready 상태는 재사용하지 않는다. Connection loss 전에 transport가
operation을 수락했는지 알 수 없으면 그 operation은 다른 peer에 제출하지 않는다. 자세한 시간과
상태 전이는 [Transport liveness §6](29-transport-liveness.ko.md#6-connection-loss와-reconnect)이
정의한다.

## 4. Object routing과 생성 recovery

### 4.1 Logical ID 메시징과 ObjectGeneration

`ObjectGeneration`을 어디에 쓰고 어디에 쓰지 않는지는
[Spot·Actor routing §2.5](18-object-routing.ko.md#25-objectgeneration을-어디에-쓰고-어디에-쓰지-않는가)가
정한다. Operation별 적용 표와 owner가 사라졌을 때의 결과가 그곳에 있다.

장애 대응에서 이 구분이 만드는 차이는 하나다. **일반 Actor·Spot message는 global logical ID만 target으로 사용한다.**
generation이 아니라 logical ID의 current Ready object를 대상으로 하므로,
object가 같은 ID로 다시 만들어진 것과 owner를 잃은 것이 서로 다른 결과로 끝난다. `ObjectGeneration`은
일반 message의 **Target 일치 조건에서 제외한다.** 전자는 새 incarnation이 처리하고 후자는
`Unavailable`이다. 아래 절들은 후자만 다룬다.

### 4.2 기존 Actor와 Spot

Actor와 Spot message는 Location Store에서 확인한 현재 `Ready` owner를 사용한다. Cache가 만료되거나
owner lease가 무효가 되면 다음 새 operation이 현재 owner를 다시 조회한다. 실패한 operation 자체는
새 owner에게 자동으로 제출하지 않는다.

계획된 relocation으로 owner가 바뀐 직후에는 이전 owner가 이미 받은 message를 commit된 target으로
전달할 수 있다. 이 동작을 [Message Follow](01-glossary.ko.md#message-follow)라고 한다.
이 전달 경로의 유지 시간을 정하는 [MessageFollowDuration](01-glossary.ko.md#message-follow-duration)의
기본값은 30초이며 0이면 사용하지 않는다. Message Follow는 이미 commit된
이동 경로를 따를 뿐 owner process 장애 뒤 새 owner를 선택하지 않으므로 failover가 아니다. 자세한
route와 cache 규칙은 [Spot·Actor routing](18-object-routing.ko.md)이 정의한다.

현재 `Ready` Actor 또는 Spot의 owner process가 종료되면 Framework는 다른 node에 같은 object를
자동 복원하지 않는다. Location Store에 기록된 owner를 임의로 바꾸거나 같은 global ID의 새
incarnation을 만들지 않는다. 이 규칙은 Instance Spot에도 동일하게 적용한다. Instance Spot이라는
종류만으로 owner lease 만료 뒤 authority를 release하거나 다음 message를 cold activation으로
전환하지 않는다.

### 4.3 Actor와 Spot 생성

Object가 없는 상태에서 생성 요청이 경쟁하면 Location Store의 `Creating` record를 먼저 확보한
target 하나만 factory를 실행한다. 생성 중 process가 종료되면 다음 Framework operation은 같은
object ID와 generation의 생성 record를 다시 확인한다. 같은 생성을 계속하거나 정확히 그 record를
취소할 수 있으며, factory는 같은 입력으로 다시 호출될 수 있다.

이 동작은 생성이 `Ready`로 공개되기 전의 recovery다. 이미 실행 중인 object의 owner 장애를
복구하는 failover가 아니다. 생성 경쟁과 결과는
[Spot과 Actor membership §2](15-spot-actor.ko.md#2-object를-하나만-생성하도록-확정하는-과정)가 정의한다.

### 4.4 Instance Spot cold activation과 owner 장애를 구분한다

최초 message가 도착했을 때 Instance Spot을 만들고 준비하는 과정을
[cold activation](01-glossary.ko.md#cold-activation)이라고 한다. Framework는 최초 message와 생성
record를 저장하므로 process가 `Creating` 또는 최초 message 복원 중에 종료돼도 같은 generation으로
생성을 계속하거나 취소할 수 있다. 최초 message를 queue 선두에 복원하기 전에는 새 message를
처리하지 않는다.

Instance Spot은 별도 create API를 호출하지 않고 `Missing` 상태에서 첫 message로 생성한다. 이 특징은
object를 **언제 만드는지** 정할 뿐, `Ready` owner 장애 뒤 다른 node에서 object를 자동 복원하는
failover 정책을 추가하지 않는다. 다음 표는 message가 도착했을 때 현재 authority에 따라 Framework가
어떤 동작을 하는지 구분한다.

| 현재 상태 | Instance intent가 있는 새 message의 처리 |
|---|---|
| Authority record가 없는 `Missing` | Eligible node 하나를 선택해 새 `ObjectGeneration`의 cold activation을 시작한다. |
| `Creating` 또는 최초 message를 아직 복원하지 않은 `Ready` | 저장한 생성 record와 최초 message를 사용해 같은 `ObjectGeneration`의 생성을 계속하거나 취소한다. 새 incarnation을 만들지 않는다. |
| Owner lease가 유효한 `Ready` | 현재 owner로 message를 보낸다. Cold activation을 시작하지 않는다. |
| `Ready` owner process가 종료되었거나 owner lease가 무효임 | Authority record를 자동 release하지 않고 다른 node에서 새 incarnation을 만들지 않는다. Operation은 `Unavailable`로 끝난다. |
| Application의 explicit `Close`가 authority release까지 완료됨 | 이후 조회 결과는 `Missing`이다. 다음 Instance intent message는 새 `ObjectGeneration`의 cold activation을 시작할 수 있다. |
| 계획된 `Relocate`가 진행 중이거나 완료됨 | Relocation 계약에 따라 같은 object와 `ObjectGeneration`을 target으로 옮긴다. Cold activation이나 crash failover로 처리하지 않는다. |

따라서 “process가 종료된 뒤 lease가 만료되면 다음 message가 다른 node에서 Instance Spot을 다시
활성화한다”는 동작은 현재 계약에 없다. 그런 동작을 제공하려면, 장애가 난 owner의 authority를 어떤
조건에서 release할지, 저장한 state와 수락된 operation을 어떻게 복구할지, 이전 owner를 어떤 fence로
차단할지를 별도의 failover 계약으로 정의해야 한다.

최초 생성 recovery 정보는 Instance Spot의 최초 생성에만 사용한다. Actor, User Spot, 이미 `Ready`인
Instance Spot과 host relocation에는 적용하지 않는다. 저장과 재개 순서는
[Location runtime §6.1](21-location-runtime.ko.md#61-message를-받은-node에서-instance-spot을-처음-만든다)이
정의한다.

## 5. Host relocation 장애

`Relocate`는 실행 중인 source와 선택한 target이 상태와 아직 실행하지 않은 작업을 넘기는 계획된
동작이다. 장애가 발생한 host를 대신할 owner를 찾는 operation이 아니다. 현재 version은 source
runtime, 선택한 target runtime, Location Store와 Relocation Store가 operation이 끝날 때까지 실행되는
graceful handoff만 지원한다.

| 실패 시점 | Framework의 처리 |
|---|---|
| Owner 변경 commit 전 | Target instance와 temporary queue를 폐기하고 source owner·membership과 queue를 유지한다. 다른 target을 자동 선택하지 않는다. |
| Store 변경 결과를 받지 못함 | 성공이나 실패를 추측하지 않고 같은 authority record를 다시 읽어 실제 owner를 확인한다. |
| Owner 변경 commit 뒤, 같은 target process가 실행 중임 | Source로 rollback하지 않는다. 같은 target에서 lifecycle callback이나 dispatch 전환을 deadline 안에 다시 시도할 수 있다. |
| Owner 변경 commit 뒤 target process가 종료됨 | Location Store의 target owner는 유지하지만 object는 `Unavailable` 상태가 된다. 다른 runtime이 relocation을 이어받지 않는다. |
| Source process 또는 target process가 operation 중 종료됨 | 다른 target 선택, process 재시작 뒤 relocation 재개와 source rollback을 수행하지 않는다. |

Commit 전에 source를 유지하는 것은 failover가 아니라 아직 owner를 바꾸지 않은 operation의 취소다.
Commit 뒤 같은 target에서 계속하는 것도 새로운 target 선택이 아니다. Process 종료 뒤 object
failover는 현재 계약에 포함되지 않는다. 자세한 단계와 결과는
[Host Relocate와 Shutdown §1.1](28-graceful-drain-handoff.ko.md#11-장애-처리-범위)과
[Spot과 Actor membership §7](15-spot-actor.ko.md#7-실패-처리-범위)이 정의한다.

## 6. Session과 binding

Actor가 계획된 relocation으로 이동하면 Session의 physical STREAM connection은 유지된다. Target
runtime이 Session owner에 위치 갱신 message를 보내 해당 Actor의 binding route와 current
`ActorRef` 위치 snapshot을 바꾼다. 같은 Actor incarnation을 식별하는
[ObjectGeneration](01-glossary.ko.md#objectgeneration)이 유지된 relocation에만 이 갱신을 적용하며
Application은 relocation을 알기 위해 다시 bind하지 않는다.

Actor를 제거하거나 owner 장애 뒤 같은 ActorId의 새 incarnation을 만들면 이전 binding은
종료된 상태로 유지한다. 일반 Actor direct message는 current ActorId로 보낼 수 있지만 Session
relay는 current binding token이 필요하므로 Application이 새 `ActorRef`를 bind해야 한다. 이전
Session에서 늦게 도착한 relay·unbind·disconnect는 새 binding에 적용하지 않는다.

Session owner process가 종료되면 Framework는 physical connection, Session identity와 binding을 다른
process로 이전하지 않는다. Client reconnect는 새 Session을 만들며, Application은 새 Session에서 인증과
bind를 다시 수행해야 한다. 이전 connection의 reply와 binding update는 새 Session에 적용하지 않는다.
자세한 종료 경계는 [Session–Actor dispatch §6](20-session-actor-dispatch.ko.md#6-failure-처리)이
정의한다.

## 7. Store 장애

Framework가 Location Store 변경 결과를 받지 못하면 성공 또는 실패를 추측하지 않는다. 같은 key와
처음 사용한 `StoreVersion`으로 record를 다시 읽고, 적용된 변경인지 확인한 뒤 필요한 경우에만 같은
Store operation을 다시 시도한다. 이 확인은 owner를 동시에 둘로 만들지 않기 위한 절차이며 다른
target을 선택하는 failover가 아니다.

`StoreFailureGrace` 동안 Framework는 마지막으로 완전히 읽은 descriptor 목록을 유지하고 기존
connection의 liveness 확인을 계속한다. 새 outbound connection은 만들지 않는다. Grace는 owner lease나
relocation deadline을 연장하지 않는다. Owner 자격이 끝나면 새 message·timer 처리와 state 변경을
중단한다. Store가 복구되면 owner와 descriptor 전체를 다시 확인한 뒤 필요한 connection 변경만
적용한다.

Store 요청의 재확인과 payload 순서는 [Location runtime §8](21-location-runtime.ko.md#8-store-응답을-받지-못했을-때)이
정의한다.

## 8. Application의 재시도 판단

Framework는 실패한 operation의 `ErrorKind`를 반환하지만 재시도 여부는 제공하지 않는다. Timeout이나
connection loss가 발생하면 remote handler의 실행 여부를 알 수 없을 수 있기 때문이다. Application은
operation의 idempotency, 업무 단위 idempotency key, 결과 조회 또는 상태 비교로 중복 영향을 막은 뒤
새 operation을 시작할지 결정한다.

Framework가 수락 전 target을 다시 선택하거나 Store 결과를 재확인하는 동작은 같은 operation의 내부
처리다. Application이 실패 결과를 받은 뒤 시작하는 새 operation과 구분한다. 자세한 오류와 완료 조건은
[Framework 오류 모델](32-framework-error-model.ko.md)이 정의한다.

## 9. 구현 및 contract test 검증 요구

- Channel select-one은 target이 operation을 수락하기 전까지만 다른 eligible server를 선택한다.
- Node direct, Actor·Spot direct와 Session binding operation은 지정한 logical identity를 다른 target으로
  바꾸지 않는다.
- Transport 수락 여부가 불분명하거나 operation이 이미 수락된 뒤에는 다른 peer에 자동 재제출하지 않는다.
- Peer 하나의 liveness failure가 다른 ready peer와 host state를 `Error`로 바꾸지 않는다.
- Reconnect는 handshake와 identity 검증을 다시 수행하고 이전 connection의 reply route, Session binding과
  ready 상태를 재사용하지 않는다.
- 생성 recovery는 같은 object ID와 generation만 계속하며 하나의 target만 factory를 실행한다.
- Actor·Spot direct message는 logical ID의 current Ready object를 대상으로 하며 ObjectGeneration
  mismatch만으로 application handler 실행을 거부하지 않는다.
- Destroy·Close, membership, relocation과 생성 recovery는 exact ObjectGeneration을 확인한다.
- Actor를 제거한 뒤 같은 ActorId로 다시 만들어도 이전 Session binding을 다시 사용하지 않는다.
- Instance Spot은 `Missing`일 때만 cold activation을 시작한다. `Ready` owner process 종료나 owner lease
  만료를 `Missing`으로 바꾸거나 cold activation으로 복구하지 않는다.
- Instance Spot cold activation recovery를 Actor, User Spot, 이미 `Ready`인 Instance Spot이나 host
  relocation에 사용하지 않는다.
- Relocation commit 전 failure는 source를 유지하고, commit 뒤 failure는 source로 rollback하지 않는다.
- Source 또는 target process 종료 뒤 다른 runtime이 relocation을 이어받거나 다른 target을 자동 선택하지
  않는다.
- Store 결과가 불분명하면 authority를 다시 읽기 전까지 source admission과 target dispatch를 열지 않는다.
- Session owner process 종료 뒤 Session과 binding을 다른 process에서 복원하지 않는다.
